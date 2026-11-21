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
 * libpng error handling for ReadPNG and WritePNG.
 *
 * libpng reports a fatal error by calling the error function of the png_struct, which must not return: the default
 * one longjmp()s to png_jmpbuf(png), which the caller must have set with setjmp() "every time you enter a new routine
 * that will call a png_*() function" (libpng-manual.txt). A setjmp() in a function that then returns leaves a jmp_buf
 * to a dead stack frame, and the next error is undefined behaviour (issue #235).
 *
 * So:
 *  - the error function of the png_structs of ReadPNG and WritePNG (PNGError::errorFn) stores libpng's message in the
 *    PNGError given as the error pointer of the png_struct, then longjmp()s to png_jmpbuf(png) (it never throws a C++
 *    exception through libpng's C frames);
 *  - every libpng call that may fail (png_read_*, png_write_*, png_set_IHDR...) is made in a small function that
 *    calls setjmp(png_jmpbuf(png)) first, in its own frame, and returns false after an error. Those functions have no
 *    local with a destructor (C++: a longjmp that skips a destructor is undefined behaviour), modify no local that they
 *    use after the longjmp (which would have to be volatile), and their caller reports PNGError::message.
 */

#ifndef IO_PNGError_h
#define IO_PNGError_h

#include <cstdio>
#include <png.h> // includes setjmp.h
#include <string>

namespace OFX {
namespace IO {
    struct PNGError {
        char message[256]; // the last error message of libpng, or empty

        PNGError() { message[0] = '\0'; }

        // The error function of the png_struct (png_create_read_struct/png_create_write_struct, with this PNGError as
        // the error pointer): store the message, and longjmp to png_jmpbuf(png). It never returns.
        static void errorFn(png_structp png, png_const_charp msg)
        {
            PNGError* e = static_cast<PNGError*>(png_get_error_ptr(png));
            if (e) {
                std::snprintf(e->message, sizeof(e->message), "%s", msg ? msg : "unknown error");
            }
            png_longjmp(png, 1);
        }

        // "libpng error: <message>" (or "libpng error" if libpng gave none)
        std::string text() const { return message[0] ? std::string("libpng error: ") + message : std::string("libpng error"); }
    };
} // namespace IO
} // namespace OFX

#endif // ifndef IO_PNGError_h
