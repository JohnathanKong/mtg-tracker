#pragma once
namespace mtg_ui {
struct Rect { int x,y,w,h; };
constexpr int kCenter=180;
constexpr int kInnerSafeRadius=167;
constexpr Rect kOptionalLeft{50,259,106,25};
constexpr Rect kOptionalRight{165,259,106,25};
constexpr Rect kEditButton{276,259,33,25};
constexpr int cornerRadiusSquared(int x,int y) {
    const int dx=x-kCenter,dy=y-kCenter;
    return dx*dx+dy*dy;
}
constexpr bool safelyInside(const Rect &r,int radius=kInnerSafeRadius) {
    const int R=radius*radius;
    return cornerRadiusSquared(r.x,r.y)<=R &&
           cornerRadiusSquared(r.x+r.w,r.y)<=R &&
           cornerRadiusSquared(r.x,r.y+r.h)<=R &&
           cornerRadiusSquared(r.x+r.w,r.y+r.h)<=R;
}
static_assert(safelyInside(kOptionalLeft),"Left optional counter clips the round screen");
static_assert(safelyInside(kOptionalRight),"Right optional counter clips the round screen");
static_assert(safelyInside(kEditButton),"Pencil Edit button clips the round screen");
}
