//
// Created by jamie on 2025/3/28.
//
#include "utils/InputFocusAnchor.h"

#include "log.h"

namespace
{
auto QueryFocusPath(const RE::GPtr<RE::GFxMovieView> &movieView, RE::GFxValue &focusPath) -> bool
{
    return movieView->Invoke("Selection.getFocus", &focusPath, nullptr, 0) && focusPath.IsString();
}

auto GetFocusMember(const RE::GPtr<RE::GFxMovieView> &movieView, const RE::GFxValue &focusPath, RE::GFxValue &focused) -> bool
{
    return movieView->GetVariable(&focused, focusPath.GetString()) && focused.IsObject();
}

bool LocalToGlobalRect(const RE::GPtr<RE::GFxMovieView> &movieView, RE::GRectF &boundaries, const char *focusPath)
{
    RE::GRenderer::Matrix identity;
    RE::GPointF           screenPoint;

    const RE::GPointF leftTop{.x = boundaries.left, .y = boundaries.top};
    const RE::GPointF rightBottom{.x = boundaries.right, .y = boundaries.bottom};
    // Both corners must convert or the rect mixes local and screen space;
    // the caller discards the rect unless this reports success.
    if (!movieView->TranslateLocalToScreen(focusPath, leftTop, &screenPoint, &identity))
    {
        return false;
    }
    boundaries.left = screenPoint.x;
    boundaries.top  = screenPoint.y;

    if (!movieView->TranslateLocalToScreen(focusPath, rightBottom, &screenPoint, &identity))
    {
        return false;
    }
    boundaries.right  = screenPoint.x;
    boundaries.bottom = screenPoint.y;
    return true;
}

bool GetBoundsRectFrom(const RE::GFxValue &bounds, RE::GRectF &rect)
{
    RE::GFxValue posX, posY, width, height;
    if (!bounds.GetMember("x", &posX) || !posX.IsNumber() || !bounds.GetMember("y", &posY) || !posY.IsNumber() ||
        !bounds.GetMember("width", &width) || !width.IsNumber() || !bounds.GetMember("height", &height) || !height.IsNumber())
    {
        return false;
    }
    rect.left   = static_cast<float>(posX.GetNumber());
    rect.top    = static_cast<float>(posY.GetNumber());
    rect.right  = static_cast<float>(width.GetNumber()) + rect.left;
    rect.bottom = static_cast<float>(height.GetNumber()) + rect.top;
    return true;
}

/**
 * @brief Find the first movie in the menu stack that has an active text input field
 * Search starts from the last focused menu index for efficiency, then falls back to a full search if needed.
 */
auto FindActiveInputMovie(RE::GPtr<RE::GFxMovieView> &movieView, Ime::InputFocusAnchor::size_type lastFocusedMenuIndex)
    -> Ime::InputFocusAnchor::size_type
{
    if (auto *ui = RE::UI::GetSingleton(); ui != nullptr)
    {
        const auto menuCount = ui->menuStack.size();
        if (lastFocusedMenuIndex < menuCount)
        {
            if (auto menu = ui->menuStack[lastFocusedMenuIndex]; menu != nullptr && menu->uiMovie != nullptr)
            {
                RE::GFxValue focusPath;
                if (QueryFocusPath(menu->uiMovie, focusPath))
                {
                    movieView = menu->uiMovie;
                    return lastFocusedMenuIndex;
                }
            }
        }

        for (auto i = menuCount - 1; i < menuCount; --i)
        {
            if (const auto menu = ui->menuStack[i]; menu != nullptr && menu->uiMovie != nullptr)
            {
                RE::GFxValue focusPath;
                if (QueryFocusPath(menu->uiMovie, focusPath))
                {
                    movieView = menu->uiMovie;
                    return i;
                }
            }
        }
        // Nothing focused: report "no cache" rather than a menu index — the
        // scan failed, so any index returned here would name an unfocused
        // menu and poison the fast path of the next query.
        return Ime::InputFocusAnchor::RE_ARRAY_SIZE_MAX;
    }
    return Ime::InputFocusAnchor::RE_ARRAY_SIZE_MAX;
}

bool ComputeScreenMetrics(const RE::GPtr<RE::GFxMovieView> &movieView, RE::GRectF &boundariesCache)
{
    if (!movieView)
    {
        return false;
    }

    RE::GFxValue focusPath;
    if (!QueryFocusPath(movieView, focusPath))
    {
        return false;
    }

    RE::GFxValue focusCharacter;
    if (!GetFocusMember(movieView, focusPath, focusCharacter))
    {
        return false;
    }

    RE::GFxValue caretIndex;
    if (!movieView->Invoke("Selection.getCaretIndex", &caretIndex, nullptr, 0) || !caretIndex.IsNumber())
    {
        return false;
    }
    // -1 means the focus has no caret (root clip, non-text object): treat the
    // query as failed instead of asking for the bounds of character -1.
    if (caretIndex.GetNumber() < 0.0)
    {
        return false;
    }

    RE::GFxValue hScroll;
    focusCharacter.GetMember("hscroll", &hScroll);
    std::array const args = {caretIndex};
    RE::GFxValue     charBoundaries;
    if (!focusCharacter.Invoke("getExactCharBoundaries", &charBoundaries, args) || !charBoundaries.IsObject())
    {
        return false;
    }

    RE::GRectF boundaries{};
    if (!GetBoundsRectFrom(charBoundaries, boundaries))
    {
        return false;
    }

    const float hScrollPos = hScroll.IsNumber() ? static_cast<float>(hScroll.GetNumber()) : 0.0F;
    // GetBoundsRectFrom computed right = unscrolled-left + width, so
    // scrolling must shift BOTH edges — shifting only `left` used to
    // widen the rect by the full scroll offset.
    boundaries.left -= hScrollPos;
    boundaries.right -= hScrollPos;

    if (!LocalToGlobalRect(movieView, boundaries, focusPath.GetString()))
    {
        return false;
    }

    boundariesCache = boundaries;
    return true;
}
} // namespace

auto Ime::InputFocusAnchor::ComputeScreenMetrics() -> bool
{
    RE::GPtr<RE::GFxMovieView> gMovieView;
    m_lastFocusedMenuIndex = FindActiveInputMovie(gMovieView, m_lastFocusedMenuIndex);
    if (gMovieView == nullptr)
    {
        // Keep the last valid bounds; a failed query must never hand out (0,0)
        // as a caret position.
        return false;
    }

    RE::GRectF freshBounds{};
    if (!::ComputeScreenMetrics(gMovieView, freshBounds))
    {
        return false;
    }

    m_cachedBounds = freshBounds;
    return true;
}
