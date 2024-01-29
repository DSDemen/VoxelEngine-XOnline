#ifndef FRONTEND_BLOCKS_PREVIEW_H_
#define FRONTEND_BLOCKS_PREVIEW_H_

#include "../typedefs.h"
#include <glm/glm.hpp>
#include <memory>

class Assets;
class ImageData;
class Atlas;
class Framebuffer;
class Batch3D;
class Block;
class Content;
class ContentGfxCache;

class BlocksPreview {
public:
    static ImageData* draw(
        const ContentGfxCache* cache,
        Framebuffer* framebuffer,
        Batch3D* batch,
        const Block* block, 
        int size);

    static std::unique_ptr<Atlas> build(
        const ContentGfxCache* cache,
        Assets* assets, 
        const Content* content);
};

#endif // FRONTEND_BLOCKS_PREVIEW_H_
