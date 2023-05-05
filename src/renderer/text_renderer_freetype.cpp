/*
 * Copyright (C) 2021 magicxqq <xqq@xqq.im>. All rights reserved.
 *
 * This file is part of libaribcaption.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <cassert>
#include <cstring>
#include <cstdint>
#include <cmath>
#include "base/scoped_holder.hpp"
#include "base/utf_helper.hpp"
#include "renderer/alphablend.hpp"
#include "renderer/canvas.hpp"
#include "renderer/text_renderer_freetype.hpp"
#include FT_STROKER_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H

namespace aribcaption {

TextRendererFreetype::TextRendererFreetype(Context& context, FontProvider& font_provider) :
      log_(GetContextLogger(context)), font_provider_(font_provider) {}

TextRendererFreetype::~TextRendererFreetype() = default;

bool TextRendererFreetype::Initialize() {
    FT_Library library;
    FT_Error error = FT_Init_FreeType(&library);
    if (error) {
        log_->e("Freetype: FT_Init_FreeType() failed");
        library_ = nullptr;
        return false;
    }

    library_ = ScopedHolder<FT_Library>(library, FT_Done_FreeType);
    return true;
}

void TextRendererFreetype::SetLanguage(uint32_t iso6392_language_code) {
    (void)iso6392_language_code;
    // No-OP
}

bool TextRendererFreetype::SetFontFamily(const std::vector<std::string>& font_family) {
    if (font_family.empty()) {
        return false;
    }

    if (!font_family_.empty() && font_family_ != font_family) {
        // Reset Freetype faces
        main_face_.Reset();
        fallback_face_.Reset();
        main_face_data_.clear();
        fallback_face_data_.clear();
        main_face_index_ = 0;
    }

    font_family_ = font_family;
    return true;
}

auto TextRendererFreetype::BeginDraw(Bitmap& target_bmp) -> TextRenderContext {
    return TextRenderContext(target_bmp);
}

void TextRendererFreetype::EndDraw(TextRenderContext& context) {
    (void)context;
    // No-op
}

constexpr FT_Tag kOpenTypeFeatureHalfWidth = FT_MAKE_TAG('h', 'w', 'i', 'd');
constexpr FT_Tag kOpenTypeScriptHiraganaKatakana = FT_MAKE_TAG('k', 'a', 'n', 'a');
constexpr FT_Tag kOpenTypeLangSysJapanese = FT_MAKE_TAG('J', 'A', 'N', ' ');

static uint16_t GetUint16(std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | static_cast<uint16_t>(data[offset + 1]));
}

static uint32_t GetUint32(std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint32_t>(
        (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
        (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]));
}

static size_t GetOffset16(std::vector<uint8_t>& data, size_t offset) {
    return static_cast<size_t>(GetUint16(data, offset));
}

static size_t GetOffset32(std::vector<uint8_t>& data, size_t offset) {
    return static_cast<size_t>(GetUint32(data, offset));
}

static int16_t GetInt16(std::vector<uint8_t>& data, size_t offset) {
    return static_cast<int16_t>(GetUint16(data, offset));
}

static FT_Tag GetTag(std::vector<uint8_t>& data, size_t offset) {
    return FT_MAKE_TAG(data[offset], data[offset + 1], data[offset + 2], data[offset + 3]);
}

static auto ReadCoverageTable(std::vector<uint8_t>& gsub, size_t offset) -> std::optional<std::vector<uint16_t>> {
    if (gsub.size() < offset + 2) {
        return std::nullopt;
    }
    uint16_t coverage_format = GetUint16(gsub, offset);
    if (coverage_format == 1) {
        // Coverage Format 1:
        // uint16       coverageFormat
        // uint16       glyphCount
        // uint16       glyphArray[glyphCount]
        if (gsub.size() < offset + 4) {
            return std::nullopt;
        }
        std::vector<uint16_t> coverage{};
        size_t glyph_count = GetUint16(gsub, offset + 2);
        size_t glyph_array_offset = offset + 4;
        for (size_t coverage_index = 0; coverage_index < glyph_count; coverage_index++) {
            if (gsub.size() < glyph_array_offset + coverage_index * 2 + 2) {
                return std::nullopt;
            }
            uint16_t glyph_id = GetUint16(gsub, glyph_array_offset + coverage_index * 2);
            coverage.push_back(glyph_id);
        }
        return std::make_optional(std::move(coverage));
    } else if (coverage_format == 2) {
        // Coverage Format 2:
        // uint16       coverageFormat
        // uint16       rangeCount
        // RangeRecord  rangeRecords[rangeCount]
        //
        // RangeRecord:
        // uint16       startGlyphID
        // uint16       endGlyphID
        // uint16       startCoverageIndex
        if (gsub.size() < offset + 4) {
            return std::nullopt;
        }
        std::vector<uint16_t> coverage{};
        size_t range_count = GetUint16(gsub, offset + 2);
        size_t range_records_offset = offset + 4;
        uint32_t coverage_index = 0;
        for (size_t range_index = 0; range_index < range_count; range_index++) {
            constexpr size_t kRangeRecordSize = 6;
            if (gsub.size() < range_records_offset + range_index * kRangeRecordSize + kRangeRecordSize) {
                return std::nullopt;
            }
            uint16_t start_glyph_id = GetUint16(gsub, range_records_offset + range_index * kRangeRecordSize);
            uint16_t end_glyph_id = GetUint16(gsub, range_records_offset + range_index * kRangeRecordSize + 2);
            uint16_t start_coverage_index = GetUint16(gsub, range_records_offset + range_index * kRangeRecordSize + 4);
            if (start_glyph_id > end_glyph_id || start_coverage_index != coverage_index) {
                return std::nullopt;
            }
            coverage_index += end_glyph_id - start_glyph_id;
            coverage_index += 1;
            for (uint16_t glyph_id = start_glyph_id; glyph_id <= end_glyph_id; glyph_id++) {
                coverage.push_back(glyph_id);
            }
        }
        return std::make_optional(std::move(coverage));
    }
    return std::nullopt;
}

static auto ReadScriptFeatureIndices(std::vector<uint8_t>& gsub,
                                     size_t script_list_offset,
                                     FT_Tag required_script_tag,
                                     FT_Tag required_lang_sys_tag) -> std::vector<uint16_t> {
    std::vector<uint16_t> feature_indices{};
    if (gsub.size() < script_list_offset + 2) {
        return {};
    }
    size_t script_count = GetUint16(gsub, script_list_offset);
    size_t script_records_offset = script_list_offset + 2;
    // ScriptList table:
    // uint16           scriptCount
    // ScriptRecord     scriptRecords[scriptCount]
    //
    // ScriptRecord:
    // Tag              scriptTag
    // Offset16         scriptOffset
    //
    // Script table:
    // Offset16         defaultLangSysOffset
    // uint16           langSysCount
    // LangSysRecord    langSysRecords[langSysCount]
    //
    // LangSysRecord:
    // Tag              langSysTag
    // Offset16         langSysOffset
    //
    // LangSys table:
    // Offset16 lookupOrderOffset
    // uint16   requiredFeatureIndex
    // uint16   featureIndexCount
    // uint16   featureIndices[featureIndexCount]
    for (size_t script_index = 0; script_index < script_count; script_index++) {
        constexpr size_t kScriptRecordSize = 6;
        size_t script_record_offset = script_records_offset + script_index * kScriptRecordSize;
        if (gsub.size() < script_record_offset + kScriptRecordSize) {
            return {};
        }
        FT_Tag script_tag = GetTag(gsub, script_record_offset);
        if (script_tag != required_script_tag) {
            continue;
        }
        size_t script_offset = script_list_offset + GetOffset16(gsub, script_record_offset + 4);
        if (gsub.size() < script_offset + 4) {
            return {};
        }
        size_t default_lang_sys_offset = script_offset + GetOffset16(gsub, script_offset);
        size_t lang_sys_count = GetUint16(gsub, script_offset + 2);
        size_t lang_sys_offset = default_lang_sys_offset;
        size_t lang_sys_records_offset = script_offset + 4;
        for (size_t lang_sys_index = 0; lang_sys_index < lang_sys_count; lang_sys_index++) {
            constexpr size_t kLangSysRecordSize = 6;
            size_t lang_sys_record_offset = lang_sys_records_offset + lang_sys_index * kLangSysRecordSize;
            if (gsub.size() < lang_sys_record_offset + kLangSysRecordSize) {
                return {};
            }
            FT_Tag lang_sys_tag = GetTag(gsub, lang_sys_record_offset);
            if (lang_sys_tag == required_lang_sys_tag) {
                lang_sys_offset = script_offset + GetUint16(gsub, lang_sys_record_offset + 4);
                break;
            }
        }
        if (lang_sys_offset == script_offset) {
            continue;
        }
        if (gsub.size() < lang_sys_offset + 6) {
            return {};
        }
        uint16_t required_feature_index = GetUint16(gsub, lang_sys_offset + 2);
        if (required_feature_index != 0xffff) {
            feature_indices.push_back(required_feature_index);
        }
        uint16_t feature_index_count = GetUint16(gsub, lang_sys_offset + 4);
        size_t feature_indices_offset = lang_sys_offset + 6;
        for (size_t i = 0; i < feature_index_count; i++) {
            if (gsub.size() < feature_indices_offset + i * 2 + 2) {
                return {};
            }
            uint16_t feature_index = GetUint16(gsub, feature_indices_offset + i * 2);
            feature_indices.push_back(feature_index);
        }
        break;
    }
    return feature_indices;
}

static auto LoadGSUBTable(FT_Face face, FT_Tag required_feature_tag, FT_Tag script_tag, FT_Tag lang_sys_tag)
    -> std::unordered_map<FT_UInt, FT_UInt> {
    std::unordered_map<FT_UInt, FT_UInt> half_width_subst_map{};
    FT_ULong gsub_size = 0;
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('G', 'S', 'U', 'B'), 0, nullptr, &gsub_size)) {
        return {};
    }
    std::vector<uint8_t> gsub(static_cast<size_t>(gsub_size));
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('G', 'S', 'U', 'B'), 0, reinterpret_cast<FT_Byte*>(gsub.data()),
                           &gsub_size)) {
        return {};
    }
    // GSUB Header:
    // uint16           majorVersion
    // uint16           minorVersion
    // Offset16         scriptListOffset
    // Offset16         featureListOffset
    // Offset16         lookupListOffset
    // Offset16         featureVariationsOffset if majorVersion = 1 and minorVersion = 1
    if (gsub_size - 2 < 8) {
        return {};
    }
    size_t script_list_offset = GetOffset16(gsub, 4);
    auto feature_indices = ReadScriptFeatureIndices(gsub, script_list_offset, script_tag, lang_sys_tag);
    // FeatureList table:
    // uint16           featureCount
    // FeatureRecord    featureRecords[featureCount]
    //
    // FeatureRecord:
    // Tag              featureTag
    // Offset16         featureOffset
    //
    // LookupList table:
    // uint16           lookupCount
    // Offset16         lookupOffsets[lookupCount]
    size_t feature_list_offset = GetOffset16(gsub, 6);
    size_t lookup_list_offset = GetOffset16(gsub, 8);
    uint16_t lookup_count = GetUint16(gsub, lookup_list_offset);
    size_t lookup_offsets_offset = lookup_list_offset + 2;
    if (gsub_size < lookup_list_offset + 2 || gsub_size < feature_list_offset + 2) {
        return {};
    }
    uint16_t feature_count = GetUint16(gsub, feature_list_offset);
    size_t feature_records_offset = feature_list_offset + 2;
    for (auto feature_index : feature_indices) {
        constexpr size_t kFeatureRecordSize = 6;
        size_t feature_record_offset = feature_records_offset + feature_index * kFeatureRecordSize;
        if (feature_index >= feature_count || gsub_size < feature_record_offset + kFeatureRecordSize) {
            return {};
        }
        FT_Tag feature_tag = GetTag(gsub, feature_record_offset);
        if (feature_tag != required_feature_tag) {
            continue;
        }
        size_t feature_offset = feature_list_offset + GetOffset16(gsub, feature_record_offset + 4);
        // Feature table:
        // Offset16     featureParamsOffset
        // uint16       lookupIndexCount
        // uint16       lookupListIndices[lookupIndexCount]
        if (gsub_size < feature_offset + 4) {
            return {};
        }
        size_t feature_params_offset = GetOffset16(gsub, feature_offset);
        if (feature_params_offset != 0) {
            // FeatureParams tables are defined only for 'cv01'-'cv99', 'size', and 'ss01'-'ss20'.
            return {};
        }
        uint16_t lookup_index_count = GetUint16(gsub, feature_offset + 2);
        size_t lookup_list_indices_offset = feature_offset + 4;
        for (size_t lookup_index = 0; lookup_index < lookup_index_count; lookup_index++) {
            if (gsub_size < lookup_list_indices_offset + lookup_index * 2 + 2) {
                return {};
            }
            size_t lookup_list_index = GetUint16(gsub, lookup_list_indices_offset + lookup_index * 2);
            if (lookup_list_index >= feature_count || gsub_size < lookup_offsets_offset + lookup_list_index * 2 + 2) {
                return {};
            }
            size_t lookup_offset =
                lookup_list_offset + GetOffset16(gsub, lookup_offsets_offset + lookup_list_index * 2);
            if (gsub_size < lookup_offset + 6) {
                return {};
            }
            // Lookup table:
            // uint16           lookupType
            // uint16           lookupFlag
            // uint16           subTableCount
            // Offset16         subtableOffsets[subTableCount]
            // uint16           markFilteringSet if lookupFlag & USE_MARK_FILTERING_SET
            uint16_t lookup_type = GetUint16(gsub, lookup_offset);
            uint16_t lookup_flag = GetUint16(gsub, lookup_offset + 2);
            size_t subtable_count = GetUint16(gsub, lookup_offset + 4);
            bool is_extension = lookup_type == 7;
            size_t subtable_offsets_offset = lookup_offset + 6;
            for (size_t subtable_index = 0; subtable_index < subtable_count; subtable_index++) {
                if (gsub_size < subtable_offsets_offset + subtable_index * 2 + 2) {
                    return {};
                }
                size_t subtable_offset =
                    lookup_offset + GetOffset16(gsub, subtable_offsets_offset + subtable_index * 2);
                if (gsub_size < subtable_offset + 2) {
                    return {};
                }
                uint16_t subst_format = GetUint16(gsub, subtable_offset);
                if (is_extension) {
                    // Extension Substitution Subtable Format 1:
                    // uint16       substFormat
                    // uint16       extensionLookupType
                    // Offset32     extensionOffset
                    if (subst_format != 1) {
                        continue;
                    }
                    if (gsub_size < subtable_offset + 8) {
                        return {};
                    }
                    uint16_t extension_lookup_type = GetUint16(gsub, subtable_offset + 2);
                    size_t extension_offset = GetOffset32(gsub, subtable_offset + 4);
                    lookup_type = extension_lookup_type;
                    subtable_offset += extension_offset;
                    if (gsub_size < subtable_offset + 2) {
                        return {};
                    }
                    subst_format = GetUint16(gsub, subtable_offset);
                }
                if (lookup_type == 1) {
                    // LookupType 1: Single Substitution Subtable
                    if (gsub_size < subtable_offset + 4) {
                        return {};
                    }
                    size_t coverage_offset = subtable_offset + GetOffset16(gsub, subtable_offset + 2);
                    auto coverage = ReadCoverageTable(gsub, coverage_offset);
                    if (!coverage) {
                        return {};
                    }
                    if (subst_format == 1) {
                        // Single Substitution Format 1:
                        // uint16   substFormat
                        // Offset16 coverageOffset
                        // int16    deltaGlyphID
                        if (gsub_size < subtable_offset + 6) {
                            return {};
                        }
                        int16_t delta_glyph_id = GetInt16(gsub, subtable_offset + 4);
                        for (auto&& glyph_id : coverage.value()) {
                            half_width_subst_map[static_cast<FT_UInt>(glyph_id)] =
                                static_cast<FT_UInt>(static_cast<uint16_t>(glyph_id + delta_glyph_id));
                        }
                    } else if (subst_format == 2) {
                        // Single Substitution Format 2:
                        // uint16   substFormat
                        // Offset16 coverageOffset
                        // uint16   glyphCount
                        // uint16   substituteGlyphIDs[glyphCount]
                        uint16_t glyph_count = GetUint16(gsub, subtable_offset + 4);
                        if (gsub_size < subtable_offset + 6) {
                            return {};
                        }
                        size_t substitute_glyph_ids_offset = subtable_offset + 6;
                        for (size_t coverage_index = 0; coverage_index < glyph_count; coverage_index++) {
                            if (gsub_size < substitute_glyph_ids_offset + coverage_index * 2 + 2) {
                                return {};
                            }
                            uint16_t substitute_glyph_id =
                                GetUint16(gsub, substitute_glyph_ids_offset + coverage_index * 2);
                            if (coverage->size() <= coverage_index) {
                                return {};
                            }
                            half_width_subst_map[static_cast<FT_UInt>(coverage.value()[coverage_index])] =
                                static_cast<FT_UInt>(substitute_glyph_id);
                        }
                    }
                }
            }
        }
        break;
    }
    return half_width_subst_map;
}

auto TextRendererFreetype::DrawChar(TextRenderContext& render_ctx, int target_x, int target_y,
                                    uint32_t ucs4, CharStyle style, ColorRGBA color, ColorRGBA stroke_color,
                                    float stroke_width, int char_width, int char_height,
                                    std::optional<UnderlineInfo> underline_info,
                                    TextRenderFallbackPolicy fallback_policy) -> TextRenderStatus {
    assert(char_height > 0);
    if (stroke_width < 0.0f) {
        stroke_width = 0.0f;
    }

    // Handle space characters
    if (ucs4 == 0x0009 || ucs4 == 0x0020 || ucs4 == 0x00A0 || ucs4 == 0x1680 ||
        ucs4 == 0x3000 || ucs4 == 0x202F || ucs4 == 0x205F || (ucs4 >= 0x2000 && ucs4 <= 0x200A)) {
        return TextRenderStatus::kOK;
    }

    if (!main_face_) {
        // If main FT_Face is not yet loaded, try load FT_Face from font_family_
        // We don't care about the codepoint (ucs4) now
        auto result = LoadFontFace(false);
        if (result.is_err()) {
            log_->e("Freetype: Cannot find valid font");
            return FontProviderErrorToStatus(result.error());
        }
        std::pair<FT_Face, size_t>& pair = result.value();
        main_face_ = ScopedHolder<FT_Face>(pair.first, FT_Done_Face);
        main_face_index_ = pair.second;
    }

    FT_Face face = main_face_;
    FT_UInt glyph_index = FT_Get_Char_Index(face, ucs4);

    if (glyph_index == 0) {
        log_->w("Freetype: Main font %s doesn't contain U+%04X", face->family_name, ucs4);

        if (fallback_policy == TextRenderFallbackPolicy::kFailOnCodePointNotFound) {
            return TextRenderStatus::kCodePointNotFound;
        }

        // Missing glyph, check fallback face
        if (fallback_face_ && (glyph_index = FT_Get_Char_Index(fallback_face_, ucs4))) {
            face = fallback_face_;
        } else if (main_face_index_ + 1 >= font_family_.size()) {
            // Fallback fonts not available
            return TextRenderStatus::kCodePointNotFound;
        } else {
            // Fallback fontface not loaded, or fallback fontface doesn't contain required codepoint
            // Load next fallback font face by specific codepoint
            auto result = LoadFontFace(true, ucs4, main_face_index_ + 1);
            if (result.is_err()) {
                log_->e("Freetype: Cannot find available fallback font for U+%04X", ucs4);
                return FontProviderErrorToStatus(result.error());
            }
            std::pair<FT_Face, size_t>& pair = result.value();
            fallback_face_ = ScopedHolder<FT_Face>(pair.first, FT_Done_Face);

            // Use this fallback fontface for rendering this time
            face = fallback_face_;
            glyph_index = FT_Get_Char_Index(face, ucs4);
            if (glyph_index == 0) {
                log_->e("Freetype: Got glyph_index == 0 for U+%04X in fallback font", ucs4);
                return TextRenderStatus::kCodePointNotFound;
            }
        }
    }

    if (char_width == char_height / 2) {
        if (face == main_face_) {
            if (!main_half_width_subst_map_) {
                main_half_width_subst_map_ = LoadGSUBTable(face, kOpenTypeFeatureHalfWidth,
                                                           kOpenTypeScriptHiraganaKatakana, kOpenTypeLangSysJapanese);
            }
        } else if (fallback_face_ && face == fallback_face_) {
            if (!fallback_half_width_subst_map_) {
                fallback_half_width_subst_map_ = LoadGSUBTable(
                    face, kOpenTypeFeatureHalfWidth, kOpenTypeScriptHiraganaKatakana, kOpenTypeLangSysJapanese);
            }
        }
        auto& subst_map = face == main_face_ ? main_half_width_subst_map_ : fallback_half_width_subst_map_;
        if (subst_map) {
            auto subst = subst_map->find(glyph_index);
            if (subst != subst_map->end()) {
                glyph_index = subst->second;
                char_width = char_height;
            }
        }
    }

    if (FT_Set_Pixel_Sizes(face, static_cast<FT_UInt>(char_width), static_cast<FT_UInt>(char_height))) {
        log_->e("Freetype: FT_Set_Pixel_Sizes failed");
        return TextRenderStatus::kOtherError;
    }

    int baseline = static_cast<int>(face->size->metrics.ascender >> 6);
    int ascender = static_cast<int>(face->size->metrics.ascender >> 6);
    int descender = static_cast<int>(face->size->metrics.descender >> 6);
    int underline = static_cast<int>(FT_MulFix(face->underline_position, face->size->metrics.x_scale) >> 6);
    int underline_thickness = static_cast<int>(FT_MulFix(face->underline_thickness, face->size->metrics.x_scale) >> 6);

    int em_height = ascender + std::abs(descender);
    int em_adjust_y = (char_height - em_height) / 2;

    if (FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_BITMAP)) {
        log_->e("Freetype: FT_Load_Glyph failed");
        return TextRenderStatus::kOtherError;
    }

    // Generate glyph bitmap for filling
    ScopedHolder<FT_Glyph> glyph_image(nullptr, FT_Done_Glyph);
    if (FT_Get_Glyph(face->glyph, &glyph_image)) {
        log_->e("Freetype: FT_Get_Glyph failed");
        return TextRenderStatus::kOtherError;
    }

    if (FT_Glyph_To_Bitmap(&glyph_image, FT_RENDER_MODE_NORMAL, nullptr, true)) {
        log_->e("Freetype: FT_Glyph_To_Bitmap failed");
        return TextRenderStatus::kOtherError;
    }

    ScopedHolder<FT_Glyph> border_glyph_image(nullptr, FT_Done_Glyph);

    // If we need stroke text (border)
    if (style & CharStyle::kCharStyleStroke && stroke_width > 0.0f) {
        // Generate glyph bitmap for stroke border
        ScopedHolder<FT_Glyph> stroke_glyph(nullptr, FT_Done_Glyph);
        if (FT_Get_Glyph(face->glyph, &stroke_glyph)) {
            log_->e("Freetype: FT_Get_Glyph failed");
            return TextRenderStatus::kOtherError;
        }

        ScopedHolder<FT_Stroker> stroker(nullptr, FT_Stroker_Done);
        FT_Stroker_New(library_, &stroker);
        FT_Stroker_Set(stroker,
                       static_cast<FT_Fixed>(stroke_width * 64),
                       FT_STROKER_LINECAP_ROUND,
                       FT_STROKER_LINEJOIN_ROUND,
                       0);

        FT_Glyph_StrokeBorder(&stroke_glyph, stroker, false, true);

        if (FT_Glyph_To_Bitmap(&stroke_glyph, FT_RENDER_MODE_NORMAL, nullptr, true)) {
            log_->e("Freetype: FT_Glyph_To_Bitmap failed");
            return TextRenderStatus::kOtherError;
        }

        border_glyph_image = std::move(stroke_glyph);
    }

    Canvas canvas(render_ctx.GetBitmap());

    // Draw Underline if required
    if ((style & kCharStyleUnderline) && underline_info && underline_thickness > 0) {
        int underline_y = target_y + baseline + em_adjust_y + std::abs(underline);
        Rect underline_rect(underline_info->start_x,
                            underline_y,
                            underline_info->start_x + underline_info->width,
                            underline_y + 1);

        int half_thickness = underline_thickness / 2;

        if (underline_thickness % 2) {  // odd number
            underline_rect.top -= half_thickness;
            underline_rect.bottom += half_thickness;
        } else {  // even number
            underline_rect.top -= half_thickness - 1;
            underline_rect.bottom += half_thickness;
        }

        canvas.DrawRect(color, underline_rect);
    }

    // Draw stroke border bitmap, if required
    if (border_glyph_image) {
        auto border_bitmap_glyph = reinterpret_cast<FT_BitmapGlyph>(border_glyph_image.Get());
        int start_x = target_x + border_bitmap_glyph->left;
        int start_y = target_y + baseline + em_adjust_y - border_bitmap_glyph->top;

        Bitmap bmp = FTBitmapToColoredBitmap(border_bitmap_glyph->bitmap, stroke_color);
        canvas.DrawBitmap(bmp, start_x, start_y);
    }

    // Draw filling bitmap
    if (glyph_image) {
        auto bitmap_glyph = reinterpret_cast<FT_BitmapGlyph>(glyph_image.Get());
        int start_x = target_x + bitmap_glyph->left;
        int start_y = target_y + baseline + em_adjust_y - bitmap_glyph->top;

        Bitmap bmp = FTBitmapToColoredBitmap(bitmap_glyph->bitmap, color);
        canvas.DrawBitmap(bmp, start_x, start_y);
    }

    return TextRenderStatus::kOK;
}

Bitmap TextRendererFreetype::FTBitmapToColoredBitmap(const FT_Bitmap& ft_bmp, ColorRGBA color) {
    Bitmap bitmap(static_cast<int>(ft_bmp.width), static_cast<int>(ft_bmp.rows), PixelFormat::kRGBA8888);

    for (uint32_t y = 0; y < ft_bmp.rows; y++) {
        uint32_t src_index = y * ft_bmp.pitch;
        const uint8_t* src = &ft_bmp.buffer[src_index];
        ColorRGBA* dest = bitmap.GetPixelAt(0, static_cast<int>(y));

        alphablend::FillLineWithAlphas(dest, src, color, ft_bmp.width);
    }

    return bitmap;
}

static bool MatchFontFamilyName(FT_Face face, const std::string& family_name) {
    FT_UInt sfnt_name_count = FT_Get_Sfnt_Name_Count(face);

    for (FT_UInt i = 0; i < sfnt_name_count; i++) {
        FT_SfntName sfnt_name{};

        if (FT_Get_Sfnt_Name(face, i, &sfnt_name)) {
            continue;
        }

        if (sfnt_name.name_id == TT_NAME_ID_FONT_FAMILY || sfnt_name.name_id == TT_NAME_ID_FULL_NAME) {
            std::string name_str;
            if (sfnt_name.platform_id == TT_PLATFORM_MICROSOFT) {
                name_str = utf::ConvertUTF16BEToUTF8(reinterpret_cast<uint16_t*>(sfnt_name.string),
                                                     sfnt_name.string_len / 2);
            } else {
                name_str = std::string(sfnt_name.string, sfnt_name.string + sfnt_name.string_len);
            }
            if (name_str == family_name) {
                return true;
            }
        }
    }

    return false;
}

auto TextRendererFreetype::LoadFontFace(bool is_fallback,
                                        std::optional<uint32_t> codepoint,
                                        std::optional<size_t> begin_index)
        -> Result<std::pair<FT_Face, size_t>, FontProviderError> {
    if (begin_index && begin_index.value() >= font_family_.size()) {
        return Err(FontProviderError::kFontNotFound);
    }

    // begin_index is optional
    size_t font_index = begin_index.value_or(0);

    const std::string& font_name = font_family_[font_index];
    auto result = font_provider_.GetFontFace(font_name, codepoint);

    while (result.is_err() && font_index + 1 < font_family_.size()) {
        // Find next suitable font
        font_index++;
        result = font_provider_.GetFontFace(font_family_[font_index], codepoint);
    }
    if (result.is_err()) {
        // Not found, return Err Result
        return Err(result.error());
    }

    FontfaceInfo& info = result.value();

    bool use_memory_data = false;
    std::vector<uint8_t>* memory_data = nullptr;
    if (!info.font_data.empty()) {
        use_memory_data = true;
        if (!is_fallback) {
            main_face_.Reset();
            main_face_data_ = std::move(info.font_data);
            memory_data = &main_face_data_;
        } else {  // is_fallback
            fallback_face_.Reset();
            fallback_face_data_ = std::move(info.font_data);
            memory_data = &fallback_face_data_;
        }
    }

    FT_Face face = nullptr;
    if (!use_memory_data) {
        if (FT_New_Face(library_, info.filename.c_str(), info.face_index, &face)) {
            return Err(FontProviderError::kFontNotFound);
        }
    } else {  // use_memory_data
        if (FT_New_Memory_Face(library_,
                               memory_data->data(),
                               static_cast<FT_Long>(memory_data->size()),
                               info.face_index,
                               &face)) {
            return Err(FontProviderError::kFontNotFound);
        }
    }

    if (info.face_index >= 0) {
        return Ok(std::make_pair(face, font_index));
    } else {
        // face_index is negative, e.g. -1, means face index is unknown
        // Find exact font face by PostScript name or Family name
        if (info.family_name.empty() && info.postscript_name.empty()) {
            log_->e("Freetype: Missing Family name / PostScript name for cases that face_index < 0");
            return Err(FontProviderError::kOtherError);
        }

        for (FT_Long i = 0; i < face->num_faces; i++) {
            FT_Done_Face(face);

            if (!use_memory_data) {
                if (FT_New_Face(library_, info.filename.c_str(), i, &face)) {
                    return Err(FontProviderError::kFontNotFound);
                }
            } else {  // use_memory_data
                if (FT_New_Memory_Face(library_,
                                       memory_data->data(),
                                       static_cast<FT_Long>(memory_data->size()),
                                       i,
                                       &face)) {
                    return Err(FontProviderError::kFontNotFound);
                }
            }

            // Find by comparing PostScript name
            if (!info.postscript_name.empty() && info.postscript_name == FT_Get_Postscript_Name(face)) {
                return Ok(std::make_pair(face, font_index));
            } else if (!info.family_name.empty() && MatchFontFamilyName(face, info.family_name)) {
                // Find by matching family name
                return Ok(std::make_pair(face, font_index));
            }
        }
        return Err(FontProviderError::kFontNotFound);
    }
}

}  // namespace aribcaption
