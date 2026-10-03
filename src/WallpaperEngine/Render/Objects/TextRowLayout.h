#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "TextLayout.h"
#include "TextShaping.h"

namespace WallpaperEngine::Render::Objects {
// Native keeps tab/LF/CR/space (mask 0x100002600) in the current
// face run; only break and trim at tab/CR/space
// (mask 0x100002200).
inline bool isTextRunWhitespace (char32_t codepoint) {
    return codepoint == '\t' || codepoint == '\n' || codepoint == '\r' || codepoint == ' ';
}

inline bool isTextBreakWhitespace (char32_t codepoint) {
    return codepoint == '\t' || codepoint == '\r' || codepoint == ' ';
}

/** Selects the face for one scalar, or nullptr when every face is exhausted. */
using TextFaceResolver = std::function<FT_Face (char32_t)>;

struct TextRowGlyph {
    FT_Face face;
    /** The cluster is absolute within the shaped row. */
    ShapedTextGlyph glyph;
    size_t runStart;
};

struct TextRowShape {
    std::vector<TextRowGlyph> glyphs;
    bool hasRtlRun = false;
};

/** Horizontal glyph ink in raster pixels: the FT_LOAD_NO_BITMAP pixel CBox
 * or the rendered bitmap extent for bitmap-only faces. */
inline bool textGlyphHorizontalInk (FT_Face face, FT_UInt index, float& low, float& high) {
    FT_BBox bounds {};
    if (textGlyphPixelBounds (face, index, bounds)) {
        low = static_cast<float> (bounds.xMin);
        high = static_cast<float> (bounds.xMax);
        return true;
    }
    if (FT_Load_Glyph (face, index, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0) return false;
    const auto slot = face->glyph;
    low = static_cast<float> (slot->bitmap_left);
    high = static_cast<float> (slot->bitmap_left + static_cast<int> (slot->bitmap.width));
    return true;
}

/** Native face runs plus the glyph filter.
 * - Exhausted scalars never change the run and shape with the run face.
 * - Whitespace and ZWJ never start a run; leading whitespace, ZWJ and
 *   exhausted scalars before the first run are not laid out.
 * - Glyph id 0 takes no position or advance unless keepNotdef is set
 *   (the native ellipsis refit measures all shaped glyphs).
 * Linux keeps colour Twemoji as a fallback, which has no whitespace. A
 * whitespace scalar that becomes glyph 0 in its run face keeps the advance
 * from the face that supplies it, so emoji followed by a space stay apart. */
inline bool isTextCombiningMark (char32_t codepoint) {
    const auto category = hb_unicode_general_category (hb_unicode_funcs_get_default (), codepoint);
    return category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK
        || category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
}

/** Linux Fontconfig can supply combining marks that the native fallback
 * table lacks; natively such a mark is exhausted, stays in the base run and
 * HarfBuzz composes it (RobotoMono A+U+0308 draws Ä). Keep a mark the run
 * face lacks in that run when the face covers the canonical composition of
 * the preceding base and its marks; otherwise the supplier face is kept. */
inline bool textRunComposesMark (std::span<const char32_t> row, size_t runStart, size_t index, FT_Face face) {
    if (!face || !isTextCombiningMark (row[index]) || FT_Get_Char_Index (face, static_cast<FT_ULong> (row[index])) != 0)
        return false;
    // row[first..index] is the mark sequence; its base is the scalar before it,
    // which must already belong to the current run.
    size_t first = index;
    while (first > runStart && isTextCombiningMark (row[first - 1])) --first;
    if (first == runStart) return false;
    const size_t base = first - 1;
    hb_unicode_funcs_t* funcs = hb_unicode_funcs_get_default ();
    hb_codepoint_t composed = row[base];
    for (size_t mark = base + 1; mark <= index; ++mark) {
        hb_codepoint_t next = 0;
        if (!hb_unicode_compose (funcs, composed, row[mark], &next)) return false;
        composed = next;
    }
    return FT_Get_Char_Index (face, static_cast<FT_ULong> (composed)) != 0;
}

inline TextRowShape shapeNativeTextRow (std::span<const char32_t> row, const TextFaceResolver& resolve,
                                        bool keepNotdef = false) {
    struct Run { size_t start; size_t end; FT_Face face; };
    std::vector<Run> runs;
    FT_Face current = nullptr;
    size_t start = 0;
    for (size_t index = 0; index < row.size (); ++index) {
        const FT_Face face = resolve (row[index]);
        if (!face) continue;
        if (face != current && row[index] != 0x200d && !isTextRunWhitespace (row[index])
            && !textRunComposesMark (row, start, index, current)) {
            if (current && start < index) runs.push_back ({start, index, current});
            start = index;
            current = face;
        }
    }
    if (current && start < row.size ()) runs.push_back ({start, row.size (), current});

    TextRowShape result;
    for (const auto& run : runs) {
        const ShapedTextRun shaped = shapeTextRow (run.face, row.subspan (run.start, run.end - run.start));
        result.hasRtlRun = result.hasRtlRun || shaped.direction == HB_DIRECTION_RTL;
        for (auto glyph : shaped.glyphs) {
            glyph.cluster += static_cast<uint32_t> (run.start);
            if (glyph.glyphIndex != 0) {
                result.glyphs.push_back ({run.face, glyph, run.start});
                continue;
            }
            // The whitespace substitution applies to drawn rows and to the
            // ellipsis trial alike, so the trial measures what is drawn.
            const char32_t codepoint = row[glyph.cluster];
            const FT_Face supplier = isTextRunWhitespace (codepoint) ? resolve (codepoint) : nullptr;
            bool substituted = false;
            if (supplier && supplier != run.face) {
                const char32_t single[] {codepoint};
                for (auto replacement : shapeTextRun (supplier, single).glyphs) {
                    if (replacement.glyphIndex == 0) continue;
                    replacement.cluster = glyph.cluster;
                    result.glyphs.push_back ({supplier, replacement, run.start});
                    substituted = true;
                }
            }
            if (!substituted && keepNotdef) result.glyphs.push_back ({run.face, glyph, run.start});
        }
    }
    return result;
}

struct TextLayoutLimits {
    /** Applied when > 0 (limitwidth set, finite maxwidth). */
    float maxWidth = 0.0f;
    /** Applied when > 0 (limitrows set). */
    int maxRows = 0;
    bool ellipsis = false;
    float letterSpacing = 0.0f;
    /** Face used by the retained Linux RTL word wrap for exhausted scalars. */
    FT_Face primary = nullptr;
};

struct TextLaidRow {
    std::vector<char32_t> text;
    std::vector<TextRowGlyph> glyphs;
};

namespace detail {
inline void trimTextRowAndEllipsis (std::vector<char32_t>& row, bool ellipsis) {
    // Native.
    while (!row.empty () && isTextBreakWhitespace (row.back ())) row.pop_back ();
    if (ellipsis && (row.empty () || row.back () != 0x2026)) row.push_back (0x2026);
}

/** The pre-native Linux word wrap, retained for rows containing an RTL run:
 * native wraps those rows by popping logical glyph tails and loses text. */
inline std::vector<std::vector<char32_t>> legacyWordWrap (std::span<const char32_t> row,
                                                          const TextFaceResolver& resolve,
                                                          const TextLayoutLimits& limits) {
    const float maxWidth = limits.maxWidth;
    const float letterSpacing = limits.letterSpacing;
    // The retained wrap measures contiguous face runs, shaping exhausted
    // scalars with the primary face, exactly as before the native layout.
    const auto faceOf = [&] (char32_t codepoint) {
        const FT_Face face = resolve (codepoint);
        return face ? face : limits.primary;
    };
    const auto shapeRuns = [&] (std::span<const char32_t> value, const auto& visit) {
        for (size_t start = 0; start < value.size ();) {
            const FT_Face face = faceOf (value[start]);
            size_t end = start + 1;
            while (end < value.size () && faceOf (value[end]) == face) ++end;
            for (const auto& glyph : shapeTextRow (face, value.subspan (start, end - start)).glyphs)
                visit (start, glyph);
            start = end;
        }
    };
    const auto measure = [&] (std::span<const char32_t> value) {
        float width = 0.0f;
        shapeRuns (value, [&] (size_t, const ShapedTextGlyph& glyph) {
            width += static_cast<float> (glyph.xAdvance26_6 >> 6) + letterSpacing;
        });
        return width;
    };
    std::vector<char32_t> wrapped;
    float rowWidth = 0.0f;
    float pendingWidth = 0.0f;
    std::vector<char32_t> pendingSpaces;
    bool rowHasWord = false;
    for (size_t index = 0; index < row.size ();) {
        const char32_t codepoint = row[index];
        if (codepoint == ' ' || codepoint == '\t') {
            pendingSpaces.push_back (codepoint);
            pendingWidth = measure (pendingSpaces);
            ++index;
            continue;
        }
        const size_t wordStart = index;
        while (index < row.size () && row[index] != ' ' && row[index] != '\t') ++index;
        const auto word = row.subspan (wordStart, index - wordStart);
        const float wordWidth = measure (word);
        if (rowHasWord && rowWidth + pendingWidth + wordWidth > maxWidth) {
            wrapped.push_back ('\n');
            rowWidth = 0.0f;
        } else {
            wrapped.insert (wrapped.end (), pendingSpaces.begin (), pendingSpaces.end ());
            rowWidth += pendingWidth;
        }
        pendingSpaces.clear ();
        pendingWidth = 0.0f;
        if (wordWidth <= maxWidth) {
            wrapped.insert (wrapped.end (), word.begin (), word.end ());
            rowWidth += wordWidth;
            rowHasWord = true;
            continue;
        }
        // A word without whitespace splits at shaped cluster boundaries.
        std::vector<size_t> cuts {0, word.size ()};
        shapeRuns (word, [&] (size_t start, const ShapedTextGlyph& glyph) {
            const size_t cluster = start + glyph.cluster;
            if (cluster > 0 && cluster < word.size ()) cuts.push_back (cluster);
        });
        std::sort (cuts.begin (), cuts.end ());
        cuts.erase (std::unique (cuts.begin (), cuts.end ()), cuts.end ());
        for (size_t start = 0; start < word.size ();) {
            size_t best = start;
            const auto firstCut = std::upper_bound (cuts.begin (), cuts.end (), start);
            if (letterSpacing >= 0.0f) {
                size_t lo = static_cast<size_t> (firstCut - cuts.begin ());
                size_t hi = lo;
                size_t step = 1;
                const auto fits = [&] (size_t cut) {
                    return rowWidth + measure (word.subspan (start, cuts[cut] - start)) <= maxWidth;
                };
                while (hi < cuts.size () && fits (hi)) {
                    best = cuts[hi];
                    lo = hi + 1;
                    hi += std::min (step, cuts.size () - hi);
                    step = step > cuts.size () / 2 ? cuts.size () : step * 2;
                }
                while (lo < hi) {
                    const size_t mid = lo + (hi - lo) / 2;
                    if (fits (mid)) {
                        best = cuts[mid];
                        lo = mid + 1;
                    } else hi = mid;
                }
            } else {
                for (auto it = firstCut; it != cuts.end (); ++it)
                    if (rowWidth + measure (word.subspan (start, *it - start)) <= maxWidth) best = *it;
            }
            if (best == start && rowWidth > 0.0f) {
                wrapped.push_back ('\n');
                rowWidth = 0.0f;
                continue;
            }
            if (best == start) best = *std::upper_bound (cuts.begin (), cuts.end (), start);
            const auto segment = word.subspan (start, best - start);
            wrapped.insert (wrapped.end (), segment.begin (), segment.end ());
            rowWidth += measure (segment);
            rowHasWord = true;
            start = best;
            if (start < word.size ()) {
                wrapped.push_back ('\n');
                rowWidth = 0.0f;
            }
        }
    }
    wrapped.insert (wrapped.end (), pendingSpaces.begin (), pendingSpaces.end ());
    std::vector<std::vector<char32_t>> rows (1);
    for (const char32_t codepoint : wrapped) {
        if (codepoint == '\n') rows.emplace_back ();
        else rows.back ().push_back (codepoint);
    }
    return rows;
}

inline std::optional<float> textEllipsisTrialRight (std::span<const char32_t> trial,
                                                    const TextFaceResolver& resolve) {
    // Measures every shaped glyph, without letter spacing.
    std::optional<float> right;
    float pen = 0.0f;
    for (const auto& glyph : shapeNativeTextRow (trial, resolve, true).glyphs) {
        float low = 0.0f, high = 0.0f;
        if (textGlyphHorizontalInk (glyph.face, glyph.glyph.glyphIndex, low, high))
            right = pen + static_cast<float> (glyph.glyph.xOffset26_6 >> 6) + high;
        pen += static_cast<float> (glyph.glyph.xAdvance26_6 >> 6);
    }
    return right;
}

/** split: drop runs starting at/after the
 * split, then pop the remaining run's glyphs from the end while their
 * cluster reaches it. Returns true when glyphs remain in that run and the
 * last popped glyph was unsafe to break: native then reprocesses the row
 * from its trimmed text. */
inline bool dropTextGlyphTail (std::vector<TextRowGlyph>& kept, size_t position) {
    while (!kept.empty ()) {
        const size_t runStart = kept.back ().runStart;
        if (position <= runStart) {
            while (!kept.empty () && kept.back ().runStart == runStart) kept.pop_back ();
            continue;
        }
        bool safe = true;
        while (!kept.empty () && kept.back ().runStart == runStart
               && kept.back ().glyph.cluster >= position) {
            safe = !kept.back ().glyph.unsafeToBreak;
            kept.pop_back ();
        }
        return !safe && !kept.empty () && kept.back ().runStart == runStart;
    }
    return false;
}
} // namespace detail

/** Native row layout over text with U+000D already removed.
 * Explicit rows are capped first, then each row is shaped and walked: the
 * ink right edge triggers a break at the previous ASCII space/tab/CR (or the
 * overflowing cluster), and the last allowed row is cut at the overflowing
 * glyph or refitted with an ellipsis. Every row record counts, including
 * rows with no glyphs. Rows containing an RTL run keep the Linux word wrap. */
inline std::vector<TextLaidRow> layoutTextRows (std::span<const char32_t> text,
                                                const TextFaceResolver& resolve,
                                                const TextLayoutLimits& limits) {
    struct QueuedRow { std::vector<char32_t> text; bool wrapped = false; };
    const bool limitWidth = std::isfinite (limits.maxWidth) && limits.maxWidth > 0.0f;
    const bool limitRows = limits.maxRows > 0;
    const auto capRows = [&] (std::vector<QueuedRow>& rows) {
        if (!limitRows || rows.size () <= static_cast<size_t> (limits.maxRows)) return;
        rows.resize (static_cast<size_t> (limits.maxRows));
        detail::trimTextRowAndEllipsis (rows.back ().text, limits.ellipsis);
    };

    std::vector<QueuedRow> rows;
    for (size_t begin = 0; begin < text.size ();) {
        if (limitRows && rows.size () >= static_cast<size_t> (limits.maxRows)) {
            detail::trimTextRowAndEllipsis (rows.back ().text, limits.ellipsis);
            break;
        }
        const size_t end = static_cast<size_t> (
            std::find (text.begin () + static_cast<std::ptrdiff_t> (begin), text.end (), U'\n') - text.begin ());
        rows.push_back ({{text.begin () + static_cast<std::ptrdiff_t> (begin),
                          text.begin () + static_cast<std::ptrdiff_t> (end)}});
        // A terminal LF creates no additional row.
        begin = end + 1;
    }
    if (rows.empty ()) rows.emplace_back ();

    std::vector<TextLaidRow> result;
    for (size_t index = 0; index < rows.size ();) {
        auto shaped = shapeNativeTextRow (rows[index].text, resolve);
        if (!limitWidth || rows[index].wrapped) {
            result.push_back ({rows[index].text, std::move (shaped.glyphs)});
            ++index;
            continue;
        }
        if (shaped.hasRtlRun) {
            auto split = detail::legacyWordWrap (rows[index].text, resolve, limits);
            std::vector<QueuedRow> replacement;
            for (auto& value : split) replacement.push_back ({std::move (value), true});
            rows.erase (rows.begin () + static_cast<std::ptrdiff_t> (index));
            rows.insert (rows.begin () + static_cast<std::ptrdiff_t> (index),
                         replacement.begin (), replacement.end ());
            capRows (rows);
            result.push_back ({rows[index].text, shapeNativeTextRow (rows[index].text, resolve).glyphs});
            ++index;
            continue;
        }

        const auto row = rows[index].text;
        std::vector<TextRowGlyph> kept;
        float pen = 0.0f;
        uint32_t previousCluster = 0;
        bool reshaped = false;
        bool reprocess = false;
        for (const auto& glyph : shaped.glyphs) {
            const uint32_t cluster = glyph.glyph.cluster;
            const auto inkRight = [&] {
                float low = 0.0f, high = 0.0f;
                if (!textGlyphHorizontalInk (glyph.face, glyph.glyph.glyphIndex, low, high)) high = 0.0f;
                return pen + static_cast<float> (glyph.glyph.xOffset26_6 >> 6) + high;
            };
            // tests the ink right edge, not the advance.
            if (!kept.empty () && !isTextBreakWhitespace (row[cluster]) && cluster != previousCluster
                && limits.maxWidth < inkRight ()) {
                if (!limitRows || index + 1 < static_cast<size_t> (limits.maxRows)) {
                    size_t position = cluster;
                    for (size_t scan = cluster; scan-- > 0;) {
                        if (isTextBreakWhitespace (row[scan])) {
                            position = scan;
                            break;
                        }
                    }
                    std::vector<char32_t> current (row.begin (), row.begin () + static_cast<std::ptrdiff_t> (position));
                    detail::trimTextRowAndEllipsis (current, false);
                    size_t next = position;
                    while (next < row.size () && isTextBreakWhitespace (row[next])) ++next;
                    // The tail pop uses the trimmed length, so every trailing
                    // space before the break leaves the row and its bounds.
                    const size_t trimmed = current.size ();
                    rows[index].text = std::move (current);
                    rows.insert (rows.begin () + static_cast<std::ptrdiff_t> (index + 1),
                                 QueuedRow {{row.begin () + static_cast<std::ptrdiff_t> (next), row.end ()}});
                    reprocess = detail::dropTextGlyphTail (kept, trimmed);
                    capRows (rows);
                } else if (limits.ellipsis) {
                    // Native then re-walks a replaced row with
                    // letter spacing, which the trial excludes; this reshapes
                    // it once instead, so positive spacing can exceed maxwidth
                    // by up to one spacing per glyph (no native probe yet).
                    size_t count = cluster;
                    for (;;) {
                        std::vector<char32_t> trial (row.begin (), row.begin () + static_cast<std::ptrdiff_t> (count));
                        trial.push_back (0x2026);
                        const auto right = detail::textEllipsisTrialRight (trial, resolve);
                        if (!right || *right <= limits.maxWidth || count == 0) break;
                        --count;
                    }
                    rows.resize (index + 1);
                    if (count == 0 || row[count - 1] == 0x2026) {
                        // No replacement; the
                        // glyphs kept before the overflow stay, without "…".
                        rows[index].text.assign (row.begin (), row.begin () + static_cast<std::ptrdiff_t> (cluster));
                    } else {
                        std::vector<char32_t> final (row.begin (), row.begin () + static_cast<std::ptrdiff_t> (count));
                        detail::trimTextRowAndEllipsis (final, true);
                        rows[index].text = std::move (final);
                        reshaped = true;
                    }
                } else {
                    rows[index].text.assign (row.begin (), row.begin () + static_cast<std::ptrdiff_t> (cluster));
                    rows.resize (index + 1);
                }
                break;
            }
            kept.push_back (glyph);
            pen += static_cast<float> (glyph.glyph.xAdvance26_6 >> 6) + limits.letterSpacing;
            previousCluster = cluster;
        }
        // the split popped an unsafe-to-break glyph, so
        // the same row is laid out again from its trimmed text.
        if (reprocess) continue;
        if (reshaped) kept = shapeNativeTextRow (rows[index].text, resolve).glyphs;
        result.push_back ({rows[index].text, std::move (kept)});
        ++index;
    }
    return result;
}
} // namespace WallpaperEngine::Render::Objects
