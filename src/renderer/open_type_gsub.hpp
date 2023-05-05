/*
 * Copyright (C) 2023 otya <otya281@gmail.com>. All rights reserved.
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

#ifndef ARIBCAPTION_OPEN_TYPE_GSUB
#define ARIBCAPTION_OPEN_TYPE_GSUB
#include <ft2build.h>
#include <unordered_map>
#include FT_TRUETYPE_TABLES_H

inline constexpr FT_Tag kOpenTypeFeatureHalfWidth = FT_MAKE_TAG('h', 'w', 'i', 'd');
inline constexpr FT_Tag kOpenTypeScriptHiraganaKatakana = FT_MAKE_TAG('k', 'a', 'n', 'a');
inline constexpr FT_Tag kOpenTypeLangSysJapanese = FT_MAKE_TAG('J', 'A', 'N', ' ');
auto LoadSingleGSUBTable(FT_Face face, FT_Tag required_feature_tag, FT_Tag script_tag, FT_Tag lang_sys_tag)
    -> std::unordered_map<FT_UInt, FT_UInt>;
#endif
