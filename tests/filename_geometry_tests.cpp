#include <gtest/gtest.h>

#include "core/scene/coordinates.h"
#include "platforms/filename_geometry.h"
#include "platforms/mirror_hit.h"
using namespace dpp;
TEST(FilenameGeometry, TwoColumnsStayCentredAndSeparateAtLeftEdge) {
    SpriteItem left{TextureId{1}, {28, 30}, {40, 40}};
    SpriteItem right{TextureId{2}, {88, 30}, {40, 40}};
    DrawList draw;
    draw.add(left);
    draw.add(right);
    const auto a = filenameLabelBounds(left, {400, 600}, &draw);
    const auto b = filenameLabelBounds(right, {400, 600}, &draw);
    EXPECT_FLOAT_EQ(a.x + a.width / 2, left.center.x);
    EXPECT_FLOAT_EQ(b.x + b.width / 2, right.center.x);
    EXPECT_GE(a.x, 0);
    EXPECT_LT(a.x + a.width, b.x);
    EXPECT_EQ(mirrorHit(draw, {a.x + a.width / 2, a.y + 10}), kNoTexture);
}
TEST(FilenameGeometry, RotatedBottomIconShowsNameAboveOnBothPlatformWidths) {
    SpriteItem sprite{TextureId{1}, {200, 575}, {40, 40}, 3.14159265f};
    for (float width : {120.f, 140.f}) {
        const auto name = filenameLabelBounds(sprite, {400, 600}, nullptr, width);
        EXPECT_LT(name.y + name.height, 555);
        EXPECT_FLOAT_EQ(name.x + name.width / 2, sprite.center.x);
    }
}
TEST(FilenameGeometry, DifferentRowsDoNotNarrowNamesAndCrowdedIconsStayHittable) {
    SpriteItem a{TextureId{1}, {200, 100}, {40, 40}};
    SpriteItem b{TextureId{2}, {210, 300}, {40, 40}};
    DrawList rows;
    rows.add(a);
    rows.add(b);
    EXPECT_FLOAT_EQ(filenameLabelBounds(a, {600, 600}, &rows).width, 120);
    b.center.y = 100;
    DrawList crowded;
    crowded.add(a);
    crowded.add(b);
    EXPECT_FLOAT_EQ(filenameLabelBounds(a, {600, 600}, &crowded).width, 24);
    EXPECT_EQ(mirrorHit(crowded, b.center), b.texture);
}

TEST(FilenameGeometry, StartupScalePreservesVisualHitAndTargetIdentity) {
    for (unsigned dpi : {96u, 120u, 144u, 192u}) {
        const auto scale = windowsDpiScale(dpi);
        const PixelRect work{-1600, 40, 320, 1040};
        const auto size = windowsWorkAreaSceneSize(work, scale);
        SpriteItem sprite{TextureId{37}, {size.x / 2, size.y - 30}, {40, 40}};
        DrawList draw;
        draw.add(sprite);
        const auto screen = windowsSceneToScreenPixels(sprite.center, work, scale);
        const auto hit =
            windowsScreenPixelsToScene(static_cast<int>(screen.x), static_cast<int>(screen.y), work, scale);
        EXPECT_EQ(mirrorHit(draw, hit), TextureId{37});
        const auto label = filenameLabelBounds(sprite, size, &draw);
        EXPECT_GE(label.x, 0);
        EXPECT_LE(label.x + label.width, size.x);
        EXPECT_LT(label.y + label.height, sprite.center.y - 20);
        EXPECT_EQ(mirrorHit(draw, rectCenter(label)), kNoTexture);
    }
}
