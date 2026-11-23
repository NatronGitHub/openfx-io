/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2021 The Natron Developers
 * (C) 2013-2018 INRIA
 *
 * openfx-io is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */
/*
 * png_corrupt_test — ReadPNG and WritePNG on libpng errors (issue #235), driven by the in-process host
 * (ofx_test_host.h), built with AddressSanitizer (make test-png-corrupt).
 *
 * libpng reports an error by longjmp()ing to the jmp_buf set with setjmp(png_jmpbuf(png)). ReadPNG set it in a
 * function that then returned, so the first libpng error after it (png_read_info() on a corrupt header, png_read_row()
 * on corrupt image data) jumped into a dead stack frame, and so did WritePNG. The test:
 *  - writes a 61x47 image with WritePNG, 8 and 16 bits (written-8.png and written-16.png, which are kept, to compare
 *    the files of two builds), and reads them with ReadPNG;
 *  - writes valid PNG files with libpng (8-bit RGB, 61x47, one IDAT chunk, and the same with IDAT chunks of 256
 *    bytes), and reads them with ReadPNG (getRegionOfDefinition and a render of the whole image succeed);
 *  - makes corrupt files from them: an invalid IHDR (zero width, bit depth 7: with a valid CRC), a bad CRC in IHDR,
 *    a file truncated in the middle of IHDR and just after it, garbage after the signature, a bad CRC in the (last)
 *    IDAT chunk and in the first of several, IDAT data truncated, corrupt zlib data in IDAT (with a valid CRC), a
 *    file that is not a PNG file (bad signature) and an empty file;
 *  - for each one, creates a ReadPNG instance (guessParamsFromFilename), asks for the image info (metadata()),
 *    the RoD (getFrameBounds) and renders the whole RoD (decode): the plug-in must not crash, and the RoD or the render
 *    must fail with a persistent error message (libpng's message for the libpng errors: "libpng error: ...");
 *  - renders an image wider than libpng's limit (1000001 pixels: png_set_IHDR fails) with WritePNG: the render must
 *    fail with libpng's message;
 *  - an image offset (oFFs chunk) whose RoD edge (x1 + width) is INT_MAX or more (#297): the RoD and the render must
 *    fail, without a signed overflow (built with -fsanitize=signed-integer-overflow, which aborts on one), and an
 *    offset that fits gives the RoD;
 *  - the Image Info of a tEXt and a zTXt chunk labels each with its own compression (#297);
 *  - an interlaced 16-bit RGBA image of 1000000x1000000 pixels (libpng's limit), read whole: ReadPNG's buffer
 *    (8 TB) can't be allocated (the allocator of AddressSanitizer, with allocator_may_return_null, returns NULL for
 *    more than 1 TB, whatever the memory; MS Windows doesn't overcommit), and the render must fail with
 *    kOfxStatErrMemory and a message (#276);
 *  - checks that no file descriptor and no memory is leaked: the corrupt files are read again 8 times, then the
 *    number of bytes allocated (__sanitizer_get_current_allocated_bytes) and the lowest free file descriptor must be
 *    the same as before.
 *
 * Usage: png_corrupt_test <scratch dir>   (needs $OCIO, see tests/Makefile)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <png.h>
#include <zlib.h>

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PNG_CORRUPT_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define PNG_CORRUPT_TEST_ASAN 1
#endif
#ifdef PNG_CORRUPT_TEST_ASAN
// (from <sanitizer/allocator_interface.h>, which GCC doesn't install in the default include path on Ubuntu 24.04; the
// ASan runtimes of GCC and clang both export it)
extern "C" size_t __sanitizer_get_current_allocated_bytes(void);
// a failed allocation returns NULL, as malloc() does, instead of aborting (the 8 TB buffer of testHugeImage)
extern "C" const char*
__asan_default_options()
{
    return "allocator_may_return_null=1";
}
#endif

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

int gFailures = 0;

void
check(bool cond,
      const char* fmt,
      ...)
{
    va_list a;
    va_start(a, fmt);
    std::printf("  %s: ", cond ? "PASS" : "FAIL");
    std::vprintf(fmt, a);
    std::printf("\n");
    va_end(a);
    if (!cond) {
        ++gFailures;
    }
}

inline OfxImageEffectHandle
H(Effect& e)
{
    return reinterpret_cast<OfxImageEffectHandle>(&e);
}

// ------------------------------------------------------------------ files

typedef std::vector<unsigned char> Bytes;

bool
writeBytes(const std::string& path,
           const Bytes& b)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = b.empty() || std::fwrite(b.data(), 1, b.size(), f) == b.size();

    return (std::fclose(f) == 0) && ok;
}

bool
readBytes(const std::string& path,
          Bytes* b)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    b->clear();
    unsigned char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        b->insert(b->end(), buf, buf + n);
    }
    std::fclose(f);

    return true;
}

// An 8-bit RGB PNG file (kW x kH, pseudo-random pixels, so that the image data doesn't compress much), written with
// libpng; idatSize: if > 0, the size of the IDAT chunks (png_set_compression_buffer_size)
const int kW = 61;
const int kH = 47;

bool
writeValidPNG(const std::string& path,
              size_t idatSize)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    std::vector<png_byte> pixels((size_t)kW * kH * 3);
    uint32_t r = 12345;
    for (png_byte& v : pixels) {
        r = r * 1103515245u + 12345u;
        v = (png_byte)(r >> 24);
    }
    std::vector<png_bytep> rows(kH);
    for (int y = 0; y < kH; ++y) {
        rows[y] = &pixels[(size_t)y * kW * 3];
    }
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, info ? &info : nullptr);
        std::fclose(file);

        return false;
    }
    png_init_io(png, file);
    if (idatSize) {
        png_set_compression_buffer_size(png, idatSize);
    }
    png_set_IHDR(png, info, kW, kH, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);

    return std::fclose(file) == 0;
}

// The chunks of a PNG file: offset of the length field, length of the data, type
struct Chunk {
    size_t offset;
    uint32_t length;
    std::string type;
};

uint32_t
be32(const Bytes& b,
     size_t o)
{
    return ((uint32_t)b[o] << 24) | ((uint32_t)b[o + 1] << 16) | ((uint32_t)b[o + 2] << 8) | (uint32_t)b[o + 3];
}

void
setBe32(Bytes& b,
        size_t o,
        uint32_t v)
{
    b[o] = (unsigned char)(v >> 24);
    b[o + 1] = (unsigned char)(v >> 16);
    b[o + 2] = (unsigned char)(v >> 8);
    b[o + 3] = (unsigned char)v;
}

std::vector<Chunk>
chunks(const Bytes& b)
{
    std::vector<Chunk> c;
    size_t o = 8;
    while (o + 12 <= b.size()) {
        Chunk k = { o, be32(b, o), std::string((const char*)&b[o + 4], 4) };
        c.push_back(k);
        o += 12 + (size_t)k.length;
    }

    return c;
}

// the CRC of chunk k (type and data), computed again (after its data was modified)
void
fixCRC(Bytes& b,
       const Chunk& k)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, &b[k.offset + 4], 4 + k.length);
    setBe32(b, k.offset + 8 + k.length, (uint32_t)crc);
}

const Chunk*
findChunk(const std::vector<Chunk>& c,
          const char* type,
          bool last = false)
{
    const Chunk* found = nullptr;
    for (const Chunk& k : c) {
        if (k.type == type) {
            found = &k;
            if (!last) {
                break;
            }
        }
    }

    return found;
}

// a chunk (length, type, data, CRC)
Bytes
makeChunk(const char* type,
          const Bytes& data)
{
    Bytes b(12 + data.size());
    setBe32(b, 0, static_cast<uint32_t>(data.size()));
    std::memcpy(&b[4], type, 4);
    std::copy(data.begin(), data.end(), b.begin() + 8);
    const Chunk k = { 0, static_cast<uint32_t>(data.size()), type };
    fixCRC(b, k);

    return b;
}

// the file png with the chunk inserted before its first IDAT chunk
Bytes
insertBeforeIDAT(const Bytes& png,
                 const Bytes& chunk)
{
    const std::vector<Chunk> c = chunks(png);
    const Chunk* idat = findChunk(c, "IDAT");
    Bytes b(png.begin(), png.begin() + idat->offset);
    b.insert(b.end(), chunk.begin(), chunk.end());
    b.insert(b.end(), png.begin() + idat->offset, png.end());

    return b;
}

// an oFFs chunk (offset in pixels)
Bytes
oFFsChunk(int32_t x,
          int32_t y)
{
    Bytes d(9, 0);
    setBe32(d, 0, static_cast<uint32_t>(x));
    setBe32(d, 4, static_cast<uint32_t>(y));
    d[8] = PNG_OFFSET_PIXEL;

    return makeChunk("oFFs", d);
}

// A corrupt file: its name, its bytes, and whether getRegionOfDefinition can succeed (the header is valid: only
// the render fails)
struct Corrupt {
    std::string name;
    Bytes bytes;
    bool headerValid;
};

std::vector<Corrupt>
makeCorruptFiles(const Bytes& one, // one IDAT chunk
                 const Bytes& many) // IDAT chunks of 256 bytes
{
    std::vector<Corrupt> v;
    const std::vector<Chunk> c1 = chunks(one);
    const std::vector<Chunk> cm = chunks(many);
    const Chunk* ihdr = findChunk(c1, "IHDR");
    const Chunk* idat = findChunk(c1, "IDAT");
    const Chunk* firstIdat = findChunk(cm, "IDAT");
    const Chunk* lastIdat = findChunk(cm, "IDAT", true);
    if (!ihdr || !idat || !firstIdat || firstIdat == lastIdat || ihdr->length != 13) {
        std::printf("FAIL: unexpected chunks in the valid files\n");
        std::exit(1);
    }
    {
        // IHDR: width 0 (with a valid CRC): png_read_info() -> png_set_IHDR() -> "Invalid IHDR data"
        Corrupt k = { "ihdr-zero-width", one, false };
        setBe32(k.bytes, ihdr->offset + 8, 0);
        fixCRC(k.bytes, *ihdr);
        v.push_back(k);
    }
    {
        // IHDR: bit depth 7 (with a valid CRC)
        Corrupt k = { "ihdr-bit-depth", one, false };
        k.bytes[ihdr->offset + 8 + 8] = 7;
        fixCRC(k.bytes, *ihdr);
        v.push_back(k);
    }
    {
        // IHDR: bad CRC ("IHDR: CRC error")
        Corrupt k = { "ihdr-crc", one, false };
        k.bytes[ihdr->offset + 8 + ihdr->length] ^= 0x5a;
        v.push_back(k);
    }
    {
        // truncated in the middle of the IHDR data ("Read Error")
        Corrupt k = { "header-truncated", Bytes(one.begin(), one.begin() + ihdr->offset + 8 + 6), false };
        v.push_back(k);
    }
    {
        // truncated just after IHDR (png_read_info() reads the next chunk)
        Corrupt k = { "header-truncated-after-ihdr", Bytes(one.begin(), one.begin() + ihdr->offset + 12 + ihdr->length), false };
        v.push_back(k);
    }
    {
        // garbage after the signature
        Corrupt k = { "garbage-after-signature", Bytes(one.begin(), one.begin() + 8), false };
        uint32_t r = 987654321u;
        for (int i = 0; i < 200; ++i) {
            r = r * 1664525u + 1013904223u;
            k.bytes.push_back((unsigned char)(r >> 24));
        }
        v.push_back(k);
    }
    {
        // bad CRC in the only IDAT chunk (detected after the last row)
        Corrupt k = { "idat-crc", one, true };
        k.bytes[idat->offset + 8 + idat->length] ^= 0x5a;
        v.push_back(k);
    }
    {
        // bad CRC in the first of several IDAT chunks (detected in the middle of the image)
        Corrupt k = { "idat-crc-first", many, true };
        k.bytes[firstIdat->offset + 8 + firstIdat->length] ^= 0x5a;
        v.push_back(k);
    }
    {
        // truncated in the middle of the IDAT data ("Read Error")
        Corrupt k = { "idat-truncated", Bytes(one.begin(), one.begin() + idat->offset + 8 + idat->length / 2), true };
        v.push_back(k);
    }
    {
        // truncated in the middle of the IDAT chunks
        Corrupt k = { "idat-truncated-many", Bytes(many.begin(), many.begin() + lastIdat->offset / 2), true };
        v.push_back(k);
    }
    {
        // corrupt zlib data in IDAT, with a valid CRC (the zlib header is kept: an inflate error, or a bad Adler-32)
        Corrupt k = { "idat-bad-zlib", one, true };
        for (uint32_t i = 2; i < idat->length; i += 3) {
            k.bytes[idat->offset + 8 + i] ^= 0xa5;
        }
        fixCRC(k.bytes, *idat);
        v.push_back(k);
    }
    {
        // not a PNG file (ReadPNG's own check, not libpng's)
        Corrupt k = { "bad-signature", one, false };
        k.bytes[1] = 'Q';
        v.push_back(k);
    }
    {
        Corrupt k = { "empty", Bytes(), false };
        v.push_back(k);
    }

    return v;
}

// ------------------------------------------------------------------ a ReadPNG instance

struct Reader {
    OfxPlugin* p = nullptr;
    Effect ctx; // context descriptor: clips and params
    Effect inst;
    bool created = false;
    Clip* out = nullptr;
    OfxRectI rod = { 0, 0, 0, 0 };
    std::vector<float> data;

    // create the instance and set the filename (guessParamsFromFilename): false if an action fails
    bool create(OfxPlugin* plugin,
                const std::string& filename)
    {
        p = plugin;
        PropSet inArgs;
        pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, H(ctx), PH(&inArgs), nullptr))) {
            std::printf("  describeInContext failed\n");

            return false;
        }
        setOverrideString(&ctx, kOfxImageEffectFileParamName, filename);
        inst.delegate = &ctx;
        setInstanceProps(inst, 100, 100, 1, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr))) {
            std::printf("  createInstance failed\n");

            return false;
        }
        created = true;
        out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
        if (!out) {
            std::printf("  no output clip\n");

            return false;
        }
        out->connected = true;
        setupClipImage(out, 1, 1, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        // the user set the filename: guessParamsFromFilename, frame range...
        if (!ok(changed(kOfxImageEffectFileParamName))) {
            std::printf("  instanceChanged(filename) failed\n");

            return false;
        }
        // an opaque RGB file, rendered as RGBA (as reader_window_test)
        setOverrideInt(&ctx, "filePremult", 0);
        setOverrideInt(&ctx, "outputPremult", 1);
        setOverrideInt(&ctx, "outputComponents", 0); // RGBA
        PropSet o;
        if (!ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, H(inst), nullptr, PH(&o)))) {
            std::printf("  getClipPreferences failed\n");

            return false;
        }

        return true;
    }

    OfxStatus changed(const char* param)
    {
        PropSet a;
        OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, param);
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. };
        pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);

        return p->mainEntry(kOfxActionInstanceChanged, H(inst), PH(&a), nullptr);
    }

    OfxStatus getRoD()
    {
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. };
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
        PropSet o;
        OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, H(inst), PH(&a), PH(&o));
        if (!ok(st)) {
            return st;
        }
        double r[4] = { 0., 0., 0., 0. };
        pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, r);
        rod = { (int)r[0], (int)r[1], (int)r[2], (int)r[3] };

        return kOfxStatOK;
    }

    // render the window (an image of the window's size, RGBA float)
    OfxStatus render(const OfxRectI& window)
    {
        const int w = window.x2 - window.x1;
        const int h = window.y2 - window.y1;
        const int rowBytes = w * 4 * (int)sizeof(float);
        data.assign((size_t)w * h * 4, 0.f);
        OfxPropertySetHandle ip = PH(&out->image);
        pSetPointer(ip, kOfxImagePropData, 0, data.data());
        int b[4] = { window.x1, window.y1, window.x2, window.y2 };
        pSetIntN(ip, kOfxImagePropBounds, 4, b);
        pSetIntN(ip, kOfxImagePropRegionOfDefinition, 4, b);
        pSetInt(ip, kOfxImagePropRowBytes, 0, rowBytes);
        double rd[4] = { (double)window.x1, (double)window.y1, (double)window.x2, (double)window.y2 };
        pSetDoubleN(PH(&out->props), kOfxImageEffectPropRegionOfDefinition, 4, rd);
        PropSet ra = makeRenderInArgs(1., 0, 0);
        pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, b);

        return p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&ra), nullptr);
    }

    ~Reader()
    {
        if (created) {
            p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
        }
    }
};

std::string
persistentError()
{
    std::lock_guard<std::mutex> lock(gMessagesMutex);

    return gPersistentError;
}

void
clearMessages()
{
    std::lock_guard<std::mutex> lock(gMessagesMutex);
    gPersistentError.clear();
    gPersistentWarning.clear();
    gMessages.clear();
}

std::string
messages()
{
    std::lock_guard<std::mutex> lock(gMessagesMutex);

    return gMessages;
}

// Read a valid file: the RoD is the image, and the render of the RoD succeeds
void
testValid(OfxPlugin* p,
          const std::string& path,
          const char* name)
{
    std::printf("== ReadPNG, valid file (%s)\n", name);
    clearMessages();
    Reader r;
    const bool created = r.create(p, path);
    check(created, "%s: create instance and set the filename", name);
    if (!created) {
        return;
    }
    const OfxStatus rodSt = r.getRoD();
    check(ok(rodSt) && r.rod.x1 == 0 && r.rod.y1 == 0 && r.rod.x2 == kW && r.rod.y2 == kH, "%s: getRegionOfDefinition = %d, %d,%d %d,%d (expected %d,%d %d,%d)",
          name, rodSt, r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2, 0, 0, kW, kH);
    const OfxStatus st = r.render(r.rod);
    const std::string err = persistentError();
    check(ok(st) && err.empty(), "%s: render = %d%s%s", name, st, err.empty() ? "" : ", error: ", err.c_str());
}

// Read a corrupt file: no crash, and getRegionOfDefinition (or, if the header is valid, the render) fails with a
// persistent error message. verbose: print the results (the first time).
void
testCorrupt(OfxPlugin* p,
            const std::string& path,
            const Corrupt& c,
            bool verbose)
{
    if (verbose) {
        std::printf("== ReadPNG, corrupt file (%s, %zu bytes)\n", c.name.c_str(), c.bytes.size());
    }
    clearMessages();
    Reader r;
    // guessParamsFromFilename (may fail: GenericReader ignores it)
    const bool created = r.create(p, path);
    if (!r.created) {
        check(false, "%s: create instance", c.name.c_str());

        return;
    }
    if (verbose) {
        std::printf("  instanceChanged(filename): %s\n", created ? "OK" : "failed");
    }
    // Image Info... (metadata()): a message, which tells the error for the corrupt headers
    clearMessages();
    r.changed("showMetadata");
    const std::string info = messages();
    if (verbose) {
        std::printf("  Image Info...: %s", info.empty() ? "(no message)\n" : info.c_str());
    }
    if (verbose && !c.headerValid && c.name != "bad-signature" && c.name != "empty") {
        check(info.find("libpng error: ") != std::string::npos, "%s: Image Info... shows libpng's error", c.name.c_str());
    }
    clearMessages();
    const OfxStatus rodSt = r.getRoD();
    std::string err = persistentError();
    if (c.headerValid) {
        if (verbose) {
            check(ok(rodSt), "%s: getRegionOfDefinition = %d (the header is valid)%s%s", c.name.c_str(), rodSt, err.empty() ? "" : ", error: ", err.c_str());
        }
        if (!ok(rodSt)) {
            return;
        }
        clearMessages();
        const OfxStatus st = r.render(r.rod);
        err = persistentError();
        if (verbose) {
            check(!ok(st) && err.find("libpng error: ") != std::string::npos, "%s: render = %d (failure), error: %s", c.name.c_str(), st, err.c_str());
        }
    } else if (verbose) {
        const char* expected = (c.name == "bad-signature" || c.name == "empty") ? "Not a PNG file" : "libpng error: ";
        check(!ok(rodSt) && err.find(expected) != std::string::npos, "%s: getRegionOfDefinition = %d (failure), error: %s", c.name.c_str(), rodSt, err.c_str());
    }
    if (!ok(rodSt)) {
        // a render anyway (the host may render after a failed RoD action, with the RoD of another frame)
        clearMessages();
        OfxRectI window = { 0, 0, kW, kH };
        const OfxStatus st = r.render(window);
        err = persistentError();
        if (verbose) {
            check(!ok(st) && !err.empty(), "%s: render = %d (failure), error: %s", c.name.c_str(), st, err.c_str());
        }
    }
}

// Render a w x h image with WritePNG (bitDepth: 0 = 8 bits, 1 = 16 bits). An image wider than libpng's limit
// (png_set_IHDR fails): the render fails with libpng's error; else it succeeds.
void
testWrite(OfxPlugin* p,
          const std::string& outFile,
          int w,
          int h,
          int bitDepth,
          bool verbose)
{
    const bool tooWide = w > 1000000; // PNG_USER_WIDTH_MAX
    if (verbose) {
        std::printf("== WritePNG, a %dx%d image, %d bits%s\n", w, h, bitDepth ? 16 : 8, tooWide ? " (wider than libpng's limit)" : "");
    }
    clearMessages();
    Effect ctx; // the descriptor (Describe and DescribeInContext, as loadAndDescribe in ofx_render_test)
    PropSet inArgs;
    pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionDescribe, H(ctx), nullptr, nullptr))
        || !ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, H(ctx), PH(&inArgs), nullptr))) {
        check(false, "WritePNG describeInContext");

        return;
    }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);
    setOverrideInt(&ctx, "bitDepth", bitDepth);
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, w, h, 1, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr))) {
        check(false, "WritePNG createInstance");

        return;
    }
    // the R/G/B/A checkboxes are hidden (as ofx_render_test's testWriteOIIONoInputImage)
    for (const char* name : { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA }) {
        auto it = ctx.params.find(name);
        if (it != ctx.params.end()) {
            pSetInt(PH(it->second.props), kOfxParamPropSecret, 0, 1);
        }
    }
    Clip* src = ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
    Clip* o = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!src || !o) {
        check(false, "WritePNG: Source and Output clips");
        p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);

        return;
    }
    src->connected = true;
    setupClipImage(src, w, h, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    float* sp = reinterpret_cast<float*>(src->buffer.data());
    for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
        sp[i] = (float)((i * 37) % 101) / 100.f; // a pattern, 0 to 1
    }
    o->connected = false;
    setupClipImage(o, w, h, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    {
        PropSet outArgs;
        p->mainEntry(kOfxImageEffectActionGetClipPreferences, H(inst), nullptr, PH(&outArgs));
    }
    PropSet a = makeRenderInArgs(1., w, h);
    const OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&a), nullptr);
    const std::string err = persistentError();
    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
    if (!verbose) {
        return;
    }
    if (tooWide) {
        check(!ok(st) && err.find("libpng error: ") != std::string::npos, "WritePNG: render = %d (failure), error: %s", st, err.c_str());
    } else {
        Bytes b;
        check(ok(st) && err.empty() && readBytes(outFile, &b) && b.size() > 8, "WritePNG: render = %d%s%s, %zu bytes written", st, err.empty() ? "" : ", error: ", err.c_str(), b.size());
    }
}

// An image offset (oFFs): the RoD is (x, y, x + kW, y + kH) if x + kW and y + kH are less than INT_MAX
// (kOfxFlagInfiniteMax, an infinite edge); else the RoD and the render fail with a persistent error message (#297)
void
testOffset(OfxPlugin* p,
           const std::string& path,
           const Bytes& one,
           int32_t x,
           int32_t y)
{
    const bool inRange = static_cast<int64_t>(x) + kW < INT_MAX && static_cast<int64_t>(y) + kH < INT_MAX;
    std::printf("== ReadPNG, oFFs offset %d,%d (%s)\n", x, y, inRange ? "in range" : "x2 or y2 would be INT_MAX or more");
    if (!writeBytes(path, insertBeforeIDAT(one, oFFsChunk(x, y)))) {
        check(false, "cannot write %s", path.c_str());

        return;
    }
    clearMessages();
    Reader r;
    r.create(p, path); // (guessParamsFromFilename fails for an offset out of range: GenericReader ignores it)
    if (!r.created) {
        check(false, "oFFs: create instance");

        return;
    }
    clearMessages();
    const OfxStatus rodSt = r.getRoD();
    std::string err = persistentError();
    if (inRange) {
        check(ok(rodSt) && r.rod.x1 == x && r.rod.y1 == y && r.rod.x2 == x + kW && r.rod.y2 == y + kH,
              "oFFs: getRegionOfDefinition = %d, %d,%d %d,%d", rodSt, r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2);

        return;
    }
    check(!ok(rodSt) && err.find("oFFs") != std::string::npos, "oFFs: getRegionOfDefinition = %d (failure), error: %s", rodSt, err.c_str());
    clearMessages();
    const OfxRectI window = { 0, 0, kW, kH };
    const OfxStatus st = r.render(window);
    err = persistentError();
    check(!ok(st) && err.find("oFFs") != std::string::npos, "oFFs: render = %d (failure), error: %s", st, err.c_str());
}

// The Image Info of a tEXt and a zTXt chunk: each is labelled with its own compression (#297)
void
testTextLabels(OfxPlugin* p,
               const std::string& path,
               const Bytes& one)
{
    std::printf("== ReadPNG, Image Info of a tEXt and a zTXt chunk\n");
    const std::string t = std::string("KeyT") + '\0' + "plain text";
    const Bytes text = makeChunk("tEXt", Bytes(t.begin(), t.end()));
    const std::string value = "compressed text";
    Bytes z(value.size() + 64);
    uLongf zLen = static_cast<uLongf>(z.size());
    if (compress(z.data(), &zLen, reinterpret_cast<const Bytef*>(value.data()), static_cast<uLong>(value.size())) != Z_OK) {
        check(false, "compress");

        return;
    }
    Bytes zd = { 'K', 'e', 'y', 'Z', 0, 0 }; // key, method 0 (deflate)
    zd.insert(zd.end(), z.begin(), z.begin() + zLen);
    if (!writeBytes(path, insertBeforeIDAT(insertBeforeIDAT(one, text), makeChunk("zTXt", zd)))) {
        check(false, "cannot write %s", path.c_str());

        return;
    }
    clearMessages();
    Reader r;
    if (!r.create(p, path)) {
        check(false, "text: create instance and set the filename");

        return;
    }
    clearMessages();
    r.changed("showMetadata");
    const std::string info = messages();
    check(info.find("KeyT (tEXt uncompressed)") != std::string::npos && info.find("KeyZ (zTXt deflate compressed)") != std::string::npos,
          "Image Info labels KeyT tEXt and KeyZ zTXt:\n%s", info.c_str());
}

// An interlaced 16-bit RGBA image of 1000000x1000000 pixels (libpng's limit), with the zlib data of the first two
// rows of the first Adam7 pass (zeros). An interlaced image is read whole, in a buffer of 8 TB: its allocation fails
// (see __asan_default_options), and the render of a small window must fail with kOfxStatErrMemory and a message,
// without writing the rows to a NULL buffer (#276).
void
testHugeImage(OfxPlugin* p,
              const std::string& path)
{
    const uint32_t n = 1000000; // PNG_USER_WIDTH_MAX, PNG_USER_HEIGHT_MAX
    std::printf("== ReadPNG, an interlaced 16-bit RGBA image of %ux%u pixels (8 TB)\n", n, n);
    Bytes ihdr(13, 0);
    setBe32(ihdr, 0, n);
    setBe32(ihdr, 4, n);
    ihdr[8] = 16;
    ihdr[9] = PNG_COLOR_TYPE_RGB_ALPHA;
    ihdr[12] = PNG_INTERLACE_ADAM7;
    const Bytes raw(2 * (1 + static_cast<size_t>(n + 7) / 8 * 8), 0); // two rows of pass 1 (filter byte 0, then zeros)
    Bytes z(compressBound(static_cast<uLong>(raw.size())));
    uLongf zLen = static_cast<uLongf>(z.size());
    if (compress(z.data(), &zLen, raw.data(), static_cast<uLong>(raw.size())) != Z_OK) {
        check(false, "compress");

        return;
    }
    z.resize(zLen);
    Bytes b = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    for (const Bytes& c : { makeChunk("IHDR", ihdr), makeChunk("IDAT", z), makeChunk("IEND", Bytes()) }) {
        b.insert(b.end(), c.begin(), c.end());
    }
    if (!writeBytes(path, b)) {
        check(false, "cannot write %s", path.c_str());

        return;
    }
    clearMessages();
    Reader r;
    if (!r.create(p, path)) {
        check(false, "huge: create instance and set the filename");

        return;
    }
    clearMessages();
    const OfxStatus rodSt = r.getRoD();
    check(ok(rodSt) && r.rod.x2 == static_cast<int>(n) && r.rod.y2 == static_cast<int>(n), "huge: getRegionOfDefinition = %d, %d,%d %d,%d", rodSt, r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2);
    clearMessages();
    const OfxRectI window = { 0, 0, 16, 16 };
    const OfxStatus st = r.render(window);
    const std::string err = persistentError();
    check(st == kOfxStatErrMemory && err.find("not enough memory") != std::string::npos, "huge: render = %d (expected kOfxStatErrMemory = %d), error: %s",
          st, kOfxStatErrMemory, err.c_str());
}

// the lowest free file descriptor (a leaked FILE keeps one)
int
lowestFreeFd()
{
#ifdef _WIN32
    int fd = _dup(0);
    if (fd >= 0) {
        _close(fd);
    }
#else
    int fd = dup(0);
    if (fd >= 0) {
        close(fd);
    }
#endif

    return fd;
}

size_t
allocatedBytes()
{
#ifdef PNG_CORRUPT_TEST_ASAN
    return __sanitizer_get_current_allocated_bytes();
#else
    return 0;
#endif
}

} // namespace

int
main(int argc,
     char** argv)
{
    if (argc != 2) {
        std::printf("usage: %s <scratch dir>\n", argv[0]);

        return 2;
    }
    const std::string dir = argv[1];
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // (the output before a crash)
    initHost();
    OfxPlugin* read = findPlugin("fr.inria.openfx.ReadPNG");
    OfxPlugin* write = findPlugin("fr.inria.openfx.WritePNG");
    if (!read || !write) {
        std::printf("FAIL: ReadPNG or WritePNG not found\n");

        return 1;
    }
    Effect readDesc, writeDesc;
    read->setHost(&gOfxHost);
    write->setHost(&gOfxHost);
    if (!ok(read->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(read->mainEntry(kOfxActionDescribe, H(readDesc), nullptr, nullptr))
        || !ok(write->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(write->mainEntry(kOfxActionDescribe, H(writeDesc), nullptr, nullptr))) {
        std::printf("FAIL: load/describe\n");

        return 1;
    }

    const std::string onePath = dir + "/valid.png";
    const std::string manyPath = dir + "/valid-many-idat.png";
    Bytes one, many;
    if (!writeValidPNG(onePath, 0) || !writeValidPNG(manyPath, 256) || !readBytes(onePath, &one) || !readBytes(manyPath, &many)) {
        std::printf("FAIL: cannot write the valid PNG files in %s\n", dir.c_str());

        return 1;
    }
    // WritePNG: valid files (written before the corrupt files are read, to compare them with those of another build)
    testWrite(write, dir + "/written-8.png", kW, kH, 0, true);
    testWrite(write, dir + "/written-16.png", kW, kH, 1, true);
    testValid(read, dir + "/written-8.png", "written by WritePNG, 8 bits");
    testValid(read, dir + "/written-16.png", "written by WritePNG, 16 bits");
    testValid(read, onePath, "one IDAT chunk");
    testValid(read, manyPath, "IDAT chunks of 256 bytes");

    const std::vector<Corrupt> corrupt = makeCorruptFiles(one, many);
    std::vector<std::string> paths;
    for (const Corrupt& c : corrupt) {
        paths.push_back(dir + "/" + c.name + ".png");
        if (!writeBytes(paths.back(), c.bytes)) {
            std::printf("FAIL: cannot write %s\n", paths.back().c_str());

            return 1;
        }
    }
    const std::string tooWide = dir + "/too-wide.png";
    for (size_t i = 0; i < corrupt.size(); ++i) {
        testCorrupt(read, paths[i], corrupt[i], true);
    }
    testWrite(write, tooWide, 1000001, 1, 0, true);
    testOffset(read, dir + "/offset.png", one, 5, 7);
    testOffset(read, dir + "/offset-max.png", one, INT_MAX - kW - 1, 7);
    testOffset(read, dir + "/offset-infinite-x.png", one, INT_MAX - kW, 7); // x2 = kOfxFlagInfiniteMax
    testOffset(read, dir + "/offset-overflow-x.png", one, INT_MAX - kW + 1, 0);
    testOffset(read, dir + "/offset-overflow-y.png", one, 0, INT_MAX);
    testTextLabels(read, dir + "/text.png", one);
#if defined(PNG_CORRUPT_TEST_ASAN) || defined(_WIN32)
    // (MS Windows doesn't overcommit: malloc() of 8 TB returns NULL)
    testHugeImage(read, dir + "/huge.png");
#else
    // (without AddressSanitizer, the 8 TB allocation may succeed, e.g. with Linux's overcommit)
    std::printf("  SKIP: the 8 TB image (not built with AddressSanitizer, not MS Windows)\n");
#endif

    // leaks: everything again (the first runs allocated the caches of the plug-ins and of the host), then the same
    // runs 8 times, after which the allocated memory and the file descriptors must be the same
    std::printf("== leaks: the corrupt files and the WritePNG error, 8 times\n");
    for (size_t i = 0; i < corrupt.size(); ++i) {
        testCorrupt(read, paths[i], corrupt[i], false);
    }
    testWrite(write, tooWide, 1000001, 1, 0, false);
    clearMessages();
    const int fd0 = lowestFreeFd();
    const size_t bytes0 = allocatedBytes();
    for (int k = 0; k < 8; ++k) {
        for (size_t i = 0; i < corrupt.size(); ++i) {
            testCorrupt(read, paths[i], corrupt[i], false);
        }
        testWrite(write, tooWide, 1000001, 1, 0, false);
    }
    clearMessages();
    const int fd1 = lowestFreeFd();
    const size_t bytes1 = allocatedBytes();
    check(fd0 == fd1, "no file descriptor leaked (lowest free descriptor: %d before, %d after)", fd0, fd1);
#ifdef PNG_CORRUPT_TEST_ASAN
    // (a leaked png_struct with its zlib stream is tens of kilobytes; 8 runs of each file)
    const long long delta = (long long)bytes1 - (long long)bytes0;
    check(delta < 1024, "no memory leaked (allocated: %zu bytes before, %zu after, %+lld)", bytes0, bytes1, delta);
#else
    (void)bytes0;
    (void)bytes1;
    std::printf("  SKIP: the allocated memory is not checked (not built with AddressSanitizer)\n");
#endif

    read->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    write->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::printf("%s (%d failure%s)\n", gFailures ? "FAIL" : "PASS", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
