#include <windows.h>
#include <gdiplus.h>
#include <stdint.h>
#include "macros/patch.h"
#include "nova.h"

// Replace the original blitters with GDI+ blitters

// Nova implements a high-quality downscaler exclusively for 16-bit. It works well but is
// inefficient and results in significant lag on Apple Silicon. We can use GDI+ to achieve
// the same quality with better performance while also covering upscaling and 24-bit.


using namespace Gdiplus;
extern "C" {

    ULONG_PTR gdiplusToken = 0;

    // Decoded UI artwork is immutable but Nova redraws it every tick. Remember
    // scaled opaque RGB555 output so repeats become ordinary row copies.
    typedef struct StretchCacheEntry {
        BYTE *source;
        BYTE *pixels;
        uint32_t hash;
        int sourceWidth, sourceHeight, sourceStride;
        QDRect sourceRect;
        int width, height;
        unsigned int age;
    } StretchCacheEntry;

    StretchCacheEntry stretchCache[8] = {};
    unsigned int stretchCacheAge = 0;

    bool cacheableStretch(NVBitmap *source, NVBitmap *dest, QDRect *srcRect, QDRect *destRect, uint32_t *hash) {
        int sourceWidth = srcRect->right - srcRect->left;
        int sourceHeight = srcRect->bottom - srcRect->top;
        int width = destRect->right - destRect->left;
        int height = destRect->bottom - destRect->top;
        if (source == dest || source->bitDepth != 16 || dest->bitDepth != 16 ||
            sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0 ||
            width * height < 4096 || (width == sourceWidth && height == sourceHeight) ||
            destRect->left < 0 || destRect->top < 0 ||
            destRect->right > dest->raw.width || destRect->bottom > dest->raw.height) return false;

        *hash = 2166136261u;
        int size = source->raw.bytesPerRow * source->raw.height;
        for (int i = 0; i < size; i++) *hash = (*hash ^ source->raw.buffer[i]) * 16777619u;
        return true;
    }

    bool useCachedStretch(NVBitmap *source, NVBitmap *dest, QDRect *srcRect, QDRect *destRect, uint32_t hash) {
        int width = destRect->right - destRect->left;
        int height = destRect->bottom - destRect->top;
        for (int i = 0; i < 8; i++) {
            StretchCacheEntry *entry = &stretchCache[i];
            if (entry->source == source->raw.buffer && entry->hash == hash &&
                entry->sourceWidth == source->raw.width && entry->sourceHeight == source->raw.height &&
                entry->sourceStride == source->raw.bytesPerRow &&
                memcmp(&entry->sourceRect, srcRect, sizeof(QDRect)) == 0 &&
                entry->width == width && entry->height == height) {
                for (int y = 0; y < height; y++) {
                    memcpy(dest->raw.buffer + (destRect->top + y) * dest->raw.bytesPerRow + destRect->left * 2,
                        entry->pixels + y * width * 2, width * 2);
                }
                entry->age = ++stretchCacheAge;
                return true;
            }
        }
        return false;
    }

    void cacheStretch(NVBitmap *source, NVBitmap *dest, QDRect *srcRect, QDRect *destRect, uint32_t hash) {
        int oldest = 0;
        for (int i = 1; i < 8; i++) {
            if (!stretchCache[i].pixels || stretchCache[i].age < stretchCache[oldest].age) oldest = i;
        }

        int width = destRect->right - destRect->left;
        int height = destRect->bottom - destRect->top;
        BYTE *pixels = (BYTE*)realloc(stretchCache[oldest].pixels, width * height * 2);
        if (!pixels) return;
        StretchCacheEntry *entry = &stretchCache[oldest];
        entry->source = source->raw.buffer;
        entry->pixels = pixels;
        entry->hash = hash;
        entry->sourceWidth = source->raw.width;
        entry->sourceHeight = source->raw.height;
        entry->sourceStride = source->raw.bytesPerRow;
        entry->sourceRect = *srcRect;
        entry->width = width;
        entry->height = height;
        entry->age = ++stretchCacheAge;
        for (int y = 0; y < height; y++) {
            memcpy(pixels + y * width * 2,
                dest->raw.buffer + (destRect->top + y) * dest->raw.bytesPerRow + destRect->left * 2, width * 2);
        }
    }

    void startupGdiplus() {
        if (!gdiplusToken) {
            GdiplusStartupInput gdiplusStartupInput;
            GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
        }
    }

    PixelFormat getPixelFormat(int bitDepth) {
        switch (bitDepth) {
            case 16:
                return PixelFormat16bppRGB555;
            case 24:
                return PixelFormat24bppRGB;
            default:
                return PixelFormat32bppRGB;
        }
    }

    void blit(Bitmap *source, NVBitmap *dest, QDRect *srcRect, QDRect *destRect) {
        auto pixelFormat = getPixelFormat(dest->bitDepth);
        Bitmap dBitmap(dest->raw.width, dest->raw.height, dest->raw.bytesPerRow, pixelFormat, dest->raw.buffer);
        Graphics graphics(&dBitmap);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        graphics.DrawImage(
            source,
            Rect(
                destRect->left,
                destRect->top,
                destRect->right - destRect->left,
                destRect->bottom - destRect->top
            ),
            srcRect->left,
            srcRect->top,
            srcRect->right - srcRect->left,
            srcRect->bottom - srcRect->top,
            UnitPixel
        );
    }

    // SETDWORD(0x00575B58, _blit16noAlpha); // 16 to 16 - existing implementation is fine here
    SETDWORD(0x00575B60, _blit16noAlpha); // 16 to 32
    SETDWORD(0x00575BBC, _blit16noAlpha); // 16 to 16 stretch
    SETDWORD(0x00575BC4, _blit16noAlpha); // 16 to 32 stretch
    void blit16(NVBitmap *source, NVBitmap *dest, QDRect *srcRect, QDRect *destRect, bool alpha) {
        startupGdiplus();

        uint32_t sourceHash = 0;
        bool cacheable = !alpha && cacheableStretch(source, dest, srcRect, destRect, &sourceHash);
        if (cacheable && useCachedStretch(source, dest, srcRect, destRect, sourceHash)) return;

        PixelFormat pixelFormat = alpha ? PixelFormat16bppARGB1555 : PixelFormat16bppRGB555;
        int stride = source->raw.bytesPerRow;
        if (stride % 4) {
            // Stride must be a multiple of 4 but the source isn't.
            // We need to copy the data into a new buffer line by line.
            stride += 4 - (stride % 4);
            BYTE *buffer = (BYTE*)malloc(stride * source->raw.height);
            BYTE *output = buffer;
            BYTE *input = source->raw.buffer;
            for (int i = 0; i < source->raw.height; i++) {
                memcpy(output, input, source->raw.bytesPerRow);
                output += stride;
                input += source->raw.bytesPerRow;
            }
            Bitmap sBitmap(source->raw.width, source->raw.height, stride, pixelFormat, buffer);
            blit(&sBitmap, dest, srcRect, destRect);
            free(buffer);
        } else {
            Bitmap sBitmap(source->raw.width, source->raw.height, stride, pixelFormat, source->raw.buffer);
            blit(&sBitmap, dest, srcRect, destRect);
        }
        if (cacheable) cacheStretch(source, dest, srcRect, destRect, sourceHash);
    }
    void blit16noAlpha(NVBitmap *source, NVBitmap *dest, QDRect *srcRect, QDRect *destRect) {
        blit16(source, dest, srcRect, destRect, false);
    }

    SETDWORD(0x00575B6C, _blit24); // 24 to 16
    SETDWORD(0x00575B74, _blit24); // 24 to 32
    SETDWORD(0x00575BD0, _blit24); // 24 to 16 stretch
    SETDWORD(0x00575BD8, _blit24); // 24 to 32 stretch
    void blit24(NVBitmap *source, NVBitmap *dest, QDRect *srcRect, QDRect *destRect) {
        startupGdiplus();

        int stride = source->raw.bytesPerRow;
        if (stride % 4) {
            stride += 4 - (stride % 4);
        }
        // Despite being labelled RGB, the bitmap we create actually operates as BGR.
        // We need to convert our RGB source data to BGR pixel by pixel.
        BYTE *buffer = (BYTE*)malloc(stride * source->raw.height);
        BYTE *output = buffer;
        BYTE *input = source->raw.buffer;
        for (int i = 0; i < source->raw.height; i++) {
            for (int j = 0; j < source->raw.width; j++) {
                output[j * 3 + 0] = input[j * 3 + 2];
                output[j * 3 + 1] = input[j * 3 + 1];
                output[j * 3 + 2] = input[j * 3 + 0];
            }
            output += stride;
            input += source->raw.bytesPerRow;
        }
        Bitmap sBitmap(source->raw.width, source->raw.height, stride, PixelFormat24bppRGB, buffer);
        blit(&sBitmap, dest, srcRect, destRect);
        free(buffer);
    }

    SETDWORD(0x00575B80, _blit32); // 32 to 16
    SETDWORD(0x00575B88, _blit32); // 32 to 32
    SETDWORD(0x00575BE4, _blit32); // 32 to 16 stretch
    SETDWORD(0x00575BEC, _blit32); // 32 to 32 stretch
    void blit32(NVBitmap *source, NVBitmap *dest, QDRect *srcRect, QDRect *destRect) {
        startupGdiplus();

        Bitmap sBitmap(source->raw.width, source->raw.height, source->raw.bytesPerRow, PixelFormat32bppRGB, source->raw.buffer);
        blit(&sBitmap, dest, srcRect, destRect);
    }

    // SETDWORD(0x00575CE8, _blit16withMask); // 16 to 16 - existing implementation is fine here
    SETDWORD(0x00575CF0, _blit16withMask); // 16 to 32
    SETDWORD(0x00575D4C, _blit16withMask); // 16 to 16 stretch
    SETDWORD(0x00575D54, _blit16withMask); // 16 to 32 stretch
    void blit16withMask(NVBitmap *source, NVBitmap *mask, NVBitmap *dest, QDRect *srcRect, QDRect *maskRect, QDRect *destRect) {
        for (int y = 0; y < source->raw.height; y++) {
            for (int x = 0; x < source->raw.width; x++) {
                int sourceIndex = y * source->raw.bytesPerRow + x * 2;
                int maskIndex = y * mask->raw.bytesPerRow + x * 2;
                // When mask is black, set the high bit to make the pixel opaque
                if (mask->raw.buffer[maskIndex] == 0) {
                    source->raw.buffer[sourceIndex + 1] |= 0x80;
                }
            }
        }
        blit16(source, dest, srcRect, destRect, true);
    }

    // SETDWORD(0x00575D10, _blit32withMask); // 32 to 16
    // SETDWORD(0x00575D18, _blit32withMask); // 32 to 32
    // SETDWORD(0x00575D74, _blit32withMask); // 32 to 16 stretch
    // SETDWORD(0x00575D7C, _blit32withMask); // 32 to 32 stretch
    // void blit32withMask(NVBitmap *source, NVBitmap *mask, NVBitmap *dest, QDRect *srcRect, QDRect *maskRect, QDRect *destRect) {
    //     startupGdiplus();

    //     for (int y = 0; y < source->raw.height; y++) {
    //         for (int x = 0; x < source->raw.width; x++) {
    //             int sourceIndex = y * source->raw.bytesPerRow + x * 4;
    //             int maskIndex = y * mask->raw.bytesPerRow + x * 4;
    //             // Use the first channel of the mask as the alpha channel of the source
    //             source->raw.buffer[sourceIndex + 3] = ~mask->raw.buffer[maskIndex];
    //         }
    //     }
    //     Bitmap sBitmap(source->raw.width, source->raw.height, source->raw.bytesPerRow, PixelFormat32bppARGB, source->raw.buffer);
    //     blit(&sBitmap, dest, srcRect, destRect);
    // }

    // Disable brightness adjustment when loading picts.
    // This prevents a forced loss of bits which would preclude dithering of 24-bit sources.
    SETBYTE(0x004B9050 + 1, 0);
};
