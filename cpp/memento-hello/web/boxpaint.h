#pragma once

#include "doc.h"

namespace web {

// Horizontal inset for a circular corner, sampled at pixel centres.
inline int roundedInset(int width, int height, int radius, int row)
{
    if (radius > width / 2) radius = width / 2;
    if (radius > height / 2) radius = height / 2;
    int edge = row < height - 1 - row ? row : height - 1 - row;
    if (radius <= 0 || edge >= radius) return 0;
    int dy = 2 * radius - 2 * edge - 1;
    int lo = 0, hi = radius;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int dx = 2 * radius - 2 * mid - 1;
        if (int64_t(dx) * dx + int64_t(dy) * dy > int64_t(4) * radius * radius) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Fill background and solid borders without painting the corner cutouts.
// The callback receives device-pixel rectangles and whether they are borders.
template<class Fill>
void paintBox(const Box &box, Fill fill)
{
    int w = box.w, h = box.h, radius = box.style.box.radius;
    if (w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    int top = box.border[0], right = box.border[1], bottom = box.border[2], left = box.border[3];
    int thick = top;
    if (right > thick) thick = right;
    if (bottom > thick) thick = bottom;
    if (left > thick) thick = left;
    int innerRadius = radius > thick ? radius - thick : 0;
    for (int y = 0; y < h;) {
        int rows = 1;
        // Group the straight middle instead of issuing one fill per pixel row.
        if (y >= radius && y >= top && y < h - radius && y < h - bottom) {
            int end = h - radius < h - bottom ? h - radius : h - bottom;
            rows = end - y;
        }
        int outer = roundedInset(w, h, radius, y);
        int start = outer, end = w - outer;
        if (y >= top && y < h - bottom && w > left + right) {
            int inner = roundedInset(w - left - right, h - top - bottom, innerRadius, y - top);
            start = left + inner; end = w - right - inner;
            if (start < outer) start = outer;
            if (end > w - outer) end = w - outer;
            if (end > start) fill(box.x + start, box.y + y, end - start, rows, false);
        } else { start = w - outer; end = w - outer; }
        if (thick && start > outer) fill(box.x + outer, box.y + y, start - outer, rows, true);
        if (thick && w - outer > end) fill(box.x + end, box.y + y, w - outer - end, rows, true);
        y += rows;
    }
}

} // namespace web
