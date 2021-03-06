// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ink/public/contrib/pdf_annotation.h"

#include "ink/engine/public/host/public_events.h"
#include "ink/engine/public/types/status_or.h"
#include "ink/pdf/io.h"
#include "ink/pdf/pdf.h"
#include "ink/pdf/pdf_engine_wrapper.h"
#include "ink/public/contrib/export.h"
#include "ink/public/contrib/import.h"
#include "ink/public/document/single_user_document.h"
#include "ink/public/document/storage/in_memory_storage.h"

static constexpr float kInterPageSpacingPoints = 10;

namespace ink {
namespace contrib {
namespace pdf {

Status LoadPdfForAnnotation(absl::string_view pdf_bytes, SEngine* engine) {
  INK_ASSIGN_OR_RETURN(auto pdf_document,
                       ::ink::pdf::Document::CreateDocument(pdf_bytes));
  proto::ExportedDocument exported_doc;
  INK_RETURN_UNLESS(ReadAndStrip(pdf_document.get(), &exported_doc));

  engine->evictAllTextures();

  auto doc =
      std::make_shared<SingleUserDocument>(std::make_shared<InMemoryStorage>());
  // Always use direct renderer for PDF editing, which is scrolling-heavy.
  engine->SetRenderingStrategy(RenderingStrategy::kDirectRenderer);
  engine->SetDocument(std::move(doc));
  contrib::ImportFromExportedDocument(
      exported_doc, contrib::ImportedPageBackgroundType::ZOOMABLE_TILES,
      ::ink::pdf::PdfEngineWrapper::CreateUriFormatString("$0"), engine);

  GLResourceManager* const gl = engine->registry()->Get<GLResourceManager>();
  const auto& texture_manager = gl->texture_manager;
  gl->background_state->SetToOutOfBoundsColor(texture_manager.get());

  auto pdf_engine_wrapper =
      std::make_shared<ink::pdf::PdfEngineWrapper>(std::move(pdf_document));
  engine->AddTextureRequestHandler("pdf", pdf_engine_wrapper);
  engine->SetSelectionProvider(pdf_engine_wrapper);

  engine->SetPageLayout(SEngine::PageLayout::VERTICAL, kInterPageSpacingPoints);
  engine->FocusOnPage(0);
  engine->registry()->Get<PanHandler>()->SetMousewheelPolicy(
      MousewheelPolicy::SCROLLS);

  // Heuristic for best tile size: a tile should be large enough to fit an
  // entire page at the default zoom.
  auto* page_manager = engine->registry()->Get<PageManager>();
  auto* cam = engine->registry()->Get<Camera>();
  int max_dim = 0;
  for (int i = 0; i < page_manager->GetNumPages(); i++) {
    const auto& info = page_manager->GetPageInfo(i);
    max_dim = std::max(
        max_dim,
        static_cast<int>(std::max(info.bounds.Width(), info.bounds.Height())));
  }
  // How many pixels wide is max page size at this zoom?
  float screen_max = cam->ConvertDistance(max_dim, DistanceType::kWorld,
                                          DistanceType::kScreen);
  static constexpr int kMaxTileSize = 2048;
  // Round up to the nearest power of 2.
  int tile_size = std::min(
      kMaxTileSize, 1 << static_cast<int>(std::ceil(std::log2(screen_max))));
  SLOG(SLOG_INFO, "tile size $0", tile_size);
  auto tile_policy = texture_manager->GetTilePolicy();
  // The PDF renderer always fills to opaque white before drawing a tile, so we
  // never need transparency.
  tile_policy.image_format = ImageFormat::BITMAP_FORMAT_RGB_888;
  tile_policy.tile_side_length = tile_size;
  texture_manager->SetTilePolicy(tile_policy);

  engine->registry()->Get<settings::Flags>()->SetFlag(
      settings::Flag::EnableMotionBlur, false);

  return OkStatus();
}

namespace {
StatusOr<ink::pdf::PdfEngineWrapper*> GetPdfEngineWrapper(
    const SEngine& engine) {
  ink::pdf::PdfEngineWrapper* wrapper;
  if (!(wrapper = dynamic_cast<ink::pdf::PdfEngineWrapper*>(
            engine.GetTextureRequestHandler("pdf")))) {
    return ErrorStatusOr<ink::pdf::PdfEngineWrapper*>(
        StatusCode::INTERNAL,
        "expected pdf texture provider to be a PdfEngineWrapper");
  }
  return StatusOr<ink::pdf::PdfEngineWrapper*>(wrapper);
}
}  // namespace

Status GetAnnotatedPdf(const SEngine& engine, std::string* out) {
  INK_ASSIGN_OR_RETURN(auto* wrapper, GetPdfEngineWrapper(engine));

  proto::ExportedDocument exported_doc;
  if (!ToExportedDocument(engine.document()->GetSnapshot(), &exported_doc)) {
    return ErrorStatus("could not export current scene state to external form");
  }
  INK_ASSIGN_OR_RETURN(auto copy, wrapper->PdfDocument()->CreateCopy());
  INK_RETURN_UNLESS(::ink::pdf::Render(exported_doc, copy.get()));
  INK_ASSIGN_OR_RETURN(*out, copy->Write<std::string>());
  return OkStatus();
}

Status GetAnnotatedPdfDestructive(const SEngine& engine, std::string* out) {
  INK_ASSIGN_OR_RETURN(auto* wrapper, GetPdfEngineWrapper(engine));
  proto::ExportedDocument exported_doc;
  if (!ToExportedDocument(engine.document()->GetSnapshot(), &exported_doc)) {
    return ErrorStatus("could not export current scene state to external form");
  }
  INK_RETURN_UNLESS(::ink::pdf::Render(exported_doc, wrapper->PdfDocument()));
  INK_ASSIGN_OR_RETURN(*out, wrapper->PdfDocument()->Write<std::string>());
  return OkStatus();
}

void SendAnnotatedPdfToHost(const SEngine& engine) {
  std::string result;
  auto status = GetAnnotatedPdf(engine, &result);
  if (status.ok()) {
    engine.registry()->Get<PublicEvents>()->PdfSaveComplete(result);
  } else {
    SLOG(SLOG_ERROR, "Could not save PDF: $0", status.error_message());
  }
}

}  // namespace pdf
}  // namespace contrib
}  // namespace ink
