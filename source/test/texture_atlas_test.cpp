#include "StarTextureAtlas.hpp"

#include "gtest/gtest.h"

using namespace Star;

namespace {

// A TextureAtlasSet backed by plain RGBA byte buffers instead of GL textures,
// so the placement and compaction logic can be exercised without a context.
// Atlas-to-atlas copies are done here exactly the way the GL backend does them
// (whole stored block, border included), which is what makes this a real check
// that dropping the CPU-side copy of every texture kept the pixels correct.
class FakeAtlasSet : public TextureAtlasSet<int> {
public:
  FakeAtlasSet(unsigned cellSize, unsigned numCells, bool supportsRegionCopy = true)
    : TextureAtlasSet<int>(cellSize, numCells), m_supportsRegionCopy(supportsRegionCopy) {}

  Vec4B pixelAt(int atlasTexture, unsigned x, unsigned y) const {
    auto const& image = m_images.get(atlasTexture);
    return image.get(x, y);
  }

  unsigned regionCopies() const {
    return m_regionCopies;
  }

protected:
  int createAtlasTexture(Vec2U const& size, PixelFormat pixelFormat) override {
    int handle = m_nextHandle++;
    m_images.add(handle, Image::filled(size, Vec4B(0, 0, 0, 0), pixelFormat));
    return handle;
  }

  void destroyAtlasTexture(int const& atlasTexture) override {
    m_images.remove(atlasTexture);
  }

  void copyAtlasPixels(int const& atlasTexture, Vec2U const& bottomLeft, Image const& image) override {
    auto& dest = m_images.get(atlasTexture);
    for (unsigned y = 0; y < image.height(); ++y)
      for (unsigned x = 0; x < image.width(); ++x)
        dest.set(bottomLeft[0] + x, bottomLeft[1] + y, image.get(x, y));
  }

  bool copyAtlasRegion(int const& destAtlasTexture, Vec2U const& destBottomLeft,
      int const& sourceAtlasTexture, RectU const& sourceRegion) override {
    if (!m_supportsRegionCopy)
      return false;

    // Read the source block out first: source and destination can be the same
    // texture and the GL backend has the same aliasing exposure.
    Image block(sourceRegion.size(), PixelFormat::RGBA32);
    auto const& source = m_images.get(sourceAtlasTexture);
    for (unsigned y = 0; y < sourceRegion.height(); ++y)
      for (unsigned x = 0; x < sourceRegion.width(); ++x)
        block.set(x, y, source.get(sourceRegion.xMin() + x, sourceRegion.yMin() + y));

    copyAtlasPixels(destAtlasTexture, destBottomLeft, block);
    ++m_regionCopies;
    return true;
  }

private:
  HashMap<int, Image> m_images;
  int m_nextHandle = 1;
  bool m_supportsRegionCopy;
  unsigned m_regionCopies = 0;
};

// Solid image whose color encodes its identity, so a moved texture can be
// told apart from a neighbour that happened to land in the same cells.
Image solidImage(unsigned size, uint8_t id) {
  return Image::filled({size, size}, Vec4B(id, 255 - id, 128, 255), PixelFormat::RGBA32);
}

void expectTextureIntact(FakeAtlasSet const& atlas, TextureAtlasSet<int>::TextureHandle const& handle, uint8_t id) {
  auto coords = handle->atlasTextureCoordinates();
  ASSERT_FALSE(handle->expired());
  for (unsigned y = coords.yMin(); y < coords.yMax(); ++y) {
    for (unsigned x = coords.xMin(); x < coords.xMax(); ++x) {
      Vec4B pixel = atlas.pixelAt(handle->atlasTexture(), x, y);
      ASSERT_EQ(pixel, Vec4B(id, 255 - id, 128, 255))
          << "texture " << (int)id << " corrupted at " << x << "," << y;
    }
  }
}

}

TEST(TextureAtlasTest, ReportsImageSizeWithoutRetainingPixels) {
  FakeAtlasSet atlas(16, 8);
  auto handle = atlas.addTexture(solidImage(30, 7));
  EXPECT_EQ(handle->imageSize(), Vec2U(30, 30));
  EXPECT_EQ(handle->atlasTextureCoordinates().size(), Vec2U(30, 30));
}

TEST(TextureAtlasTest, WritesPixelsInsideTheBorder) {
  FakeAtlasSet atlas(16, 8);
  auto handle = atlas.addTexture(solidImage(20, 42));
  expectTextureIntact(atlas, handle, 42);
}

namespace {
  // Fills one 4x4-cell page with single-cell textures, then puts one more on a
  // second page and punches a hole in the first. Compaction now has to MOVE
  // the straggler rather than just reclaim an already-empty page.
  unsigned const CellSize = 16;
  unsigned const PageCells = 4;

  void buildFullPagePlusOne(FakeAtlasSet& atlas,
      List<TextureAtlasSet<int>::TextureHandle>& handles, uint8_t idBase) {
    for (unsigned i = 0; i < PageCells * PageCells + 1; ++i)
      handles.append(atlas.addTexture(solidImage(CellSize - 2, idBase + i)));
  }
}

TEST(TextureAtlasTest, CompactionMovesPixelsAndFreesAnAtlas) {
  FakeAtlasSet atlas(CellSize, PageCells);
  List<TextureAtlasSet<int>::TextureHandle> handles;
  buildFullPagePlusOne(atlas, handles, 10);
  ASSERT_EQ(atlas.totalAtlases(), 2u);

  auto straggler = handles.last();
  atlas.freeTexture(handles[3]);
  atlas.compressionPass();

  // The straggler moved into the hole and its now-empty page was reclaimed.
  EXPECT_EQ(atlas.totalAtlases(), 1u);
  EXPECT_GT(atlas.regionCopies(), 0u);

  // Every survivor must still read back exactly what was uploaded -- there is
  // no CPU copy left to restore them from if the GPU-side move lost pixels.
  expectTextureIntact(atlas, straggler, (uint8_t)(10 + PageCells * PageCells));
  for (size_t i = 0; i < handles.size() - 1; ++i) {
    if (i == 3)
      continue;
    expectTextureIntact(atlas, handles[i], (uint8_t)(10 + i));
  }
}

TEST(TextureAtlasTest, CompactionIsSkippedWhenTheBackendCannotCopy) {
  FakeAtlasSet atlas(CellSize, PageCells, /* supportsRegionCopy */ false);
  List<TextureAtlasSet<int>::TextureHandle> handles;
  buildFullPagePlusOne(atlas, handles, 60);

  auto straggler = handles.last();
  atlas.freeTexture(handles[3]);

  atlas.compressionPass();
  // Nothing moves, so no page is reclaimed -- but nothing is corrupted either,
  // and the set must not keep retrying a copy the backend already refused.
  EXPECT_EQ(atlas.totalAtlases(), 2u);
  EXPECT_EQ(atlas.regionCopies(), 0u);
  expectTextureIntact(atlas, straggler, (uint8_t)(60 + PageCells * PageCells));

  atlas.compressionPass();
  EXPECT_EQ(atlas.regionCopies(), 0u);
  expectTextureIntact(atlas, straggler, (uint8_t)(60 + PageCells * PageCells));
}

TEST(TextureAtlasTest, EmptyAtlasesAreReclaimedWithoutCopying) {
  // 30px + border fills a whole 2x2-cell page, so each texture gets its own.
  FakeAtlasSet atlas(16, 2);

  auto first = atlas.addTexture(solidImage(30, 12));
  auto second = atlas.addTexture(solidImage(30, 22));
  ASSERT_EQ(atlas.totalAtlases(), 2u);

  atlas.freeTexture(second);

  EXPECT_EQ(atlas.totalAtlases(), 1u);
  EXPECT_EQ(atlas.regionCopies(), 0u);
  expectTextureIntact(atlas, first, 12);
}
