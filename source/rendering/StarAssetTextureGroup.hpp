#pragma once

#include "StarMaybe.hpp"
#include "StarString.hpp"
#include "StarBiMap.hpp"
#include "StarListener.hpp"
#include "StarRenderer.hpp"
#include "StarAssetPath.hpp"

namespace Star {

STAR_CLASS(AssetTextureGroup);

// Creates a renderer texture group for textures loaded directly from Assets.
class AssetTextureGroup {
public:
  // Creates a texture group using the given renderer and textureFiltering for
  // the managed textures.
  AssetTextureGroup(TextureGroupPtr textureGroup);

  // Load the given texture into the texture group if it is not loaded, and
  // return the texture pointer.
  TexturePtr loadTexture(AssetPath const& imagePath);

  // If the texture is loaded and ready, returns the texture pointer, otherwise
  // queues the texture using Assets::tryImage and returns nullptr.
  TexturePtr tryTexture(AssetPath const& imagePath);

  // Has the texture been loaded?
  bool textureLoaded(AssetPath const& imagePath) const;

  // Frees textures that haven't been used in more than 'textureTimeout' time.
  // If Root has been reloaded, will simply clear the texture group.
  void cleanup(int64_t textureTimeout);

private:
  // Returns the texture parameters.  If tryTexture is true, then returns none
  // if the texture is not loaded, and queues it, otherwise loads texture
  // immediately
  TexturePtr loadTexture(AssetPath const& imagePath, bool tryTexture);

  TextureGroupPtr m_textureGroup;
  HashMap<AssetPath, pair<TexturePtr, int64_t>> m_textureMap;
  // Deduplication only needs image IDENTITY, never image pixels. Holding an
  // ImageConstPtr here used to pin every decompressed source image in the
  // Assets cache for as long as its texture lived (Assets keeps anything whose
  // shared_ptr is not unique), so each on-screen sprite cost a full RGBA copy
  // in RAM on top of its atlas copy. The weak_ptr keeps the identity check
  // exact -- a raw key can be reused by a later allocation, so a hit is only
  // trusted when the weak_ptr still locks to the very same image.
  HashMap<Image const*, pair<weak_ptr<Image const>, TexturePtr>> m_textureDeduplicationMap;
  TrackerListenerPtr m_reloadTracker;
};

}
