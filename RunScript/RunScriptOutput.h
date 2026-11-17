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
 * OFX RunScript plugin: splitting the standard error of the script into lines, with bounded memory
 * (used on all systems: by RunScriptWin32.cpp on MS Windows, with pstream elsewhere).
 */

#ifndef IO_RunScriptOutput_h
#define IO_RunScriptOutput_h

#include <cstddef>
#include <streambuf>
#include <string>
#include <vector>

namespace RunScriptOutput {

// remove an incomplete UTF-8 sequence at the end of s (e.g. after s was cut)
inline void
dropPartialUtf8Tail(std::string* s)
{
    // the start of the last sequence: at most 3 continuation bytes (10xxxxxx) before it
    std::size_t start = s->size();
    std::size_t cont = 0;
    while (start > 0 && cont < 4 && (static_cast<unsigned char>((*s)[start - 1]) & 0xC0) == 0x80) {
        --start;
        ++cont;
    }
    if (start == 0) {
        return; // only continuation bytes: not UTF-8, leave it as it is
    }
    const unsigned char lead = static_cast<unsigned char>((*s)[start - 1]);
    const std::size_t len = (lead & 0x80) == 0 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
    if (len > cont + 1) {
        s->erase(start - 1); // the sequence is incomplete
    }
}

// s, cut to at most maxBytes bytes (plus "..." if it was cut), without cutting a UTF-8 sequence
inline std::string
truncateUtf8(const std::string& s,
             std::size_t maxBytes)
{
    if (s.size() <= maxBytes) {
        return s;
    }
    std::string t = s.substr(0, maxBytes);
    dropPartialUtf8Tail(&t);

    return t + "...";
}

// Splits the output of a process into lines, as a terminal would show them:
// - a line ends with "\n" or "\r\n";
// - a lone "\r" (progress output, as printed by ffmpeg, curl or tqdm) returns to the start of the line, so the
//   line is what follows the last "\r" (the text before it is dropped, rather than overwritten character by
//   character); a "\r" at the end of the output or before another "\r" does not drop anything;
// - a line is cut after maxLineBytes bytes (the rest of it is dropped until the next line break or lone "\r");
// - only the last lines are kept: at least maxLines lines, or fewer if they are more than maxBytes bytes in total
//   (at least the last line is always kept).
// Memory is bounded by about 2 * (maxBytes + maxLines * sizeof(std::string)) + maxLineBytes.
class LineCollector
{
public:
    explicit LineCollector(std::vector<std::string>* lines,
                           std::size_t maxLines = 1000,
                           std::size_t maxBytes = 1024 * 1024,
                           std::size_t maxLineBytes = 64 * 1024)
        : _lines(lines)
        , _maxLines(maxLines ? maxLines : 1)
        , _maxBytes(maxBytes)
        , _maxLineBytes(maxLineBytes)
        , _bytes(0)
        , _cr(false)
        , _cut(false)
    {
        for (std::size_t i = 0; i < _lines->size(); ++i) {
            _bytes += (*_lines)[i].size();
        }
    }

    void add(const char* data,
             std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i) {
            const char c = data[i];
            if (c == '\n') {
                _cr = false;
                endLine();
            } else if (c == '\r') {
                _cr = true; // a line break if followed by '\n', else a return to the start of the line
            } else {
                if (_cr) {
                    // a lone '\r': what follows replaces the line
                    _cr = false;
                    _pending.clear();
                    _cut = false;
                }
                if (_pending.size() < _maxLineBytes) {
                    _pending.push_back(c);
                } else {
                    _cut = true;
                }
            }
        }
    }

    // to be called at the end of the output: adds the last line, if it does not end with a line break
    void finish()
    {
        if (!_pending.empty() || _cut) {
            endLine();
        }
        _cr = false;
    }

private:
    void endLine()
    {
        std::string line;
        line.swap(_pending);
        if (_cut) {
            dropPartialUtf8Tail(&line);
            line += "...";
            _cut = false;
        }
        _bytes += line.size();
        _lines->push_back(std::string());
        _lines->back().swap(line);
        // erasing from the front of the vector is linear: only do it when twice over the limits
        if (_lines->size() > 2 * _maxLines || _bytes > 2 * _maxBytes) {
            std::size_t first = _lines->size() > _maxLines ? _lines->size() - _maxLines : 0;
            std::size_t bytes = 0;
            for (std::size_t i = first; i < _lines->size(); ++i) {
                bytes += (*_lines)[i].size();
            }
            while (bytes > _maxBytes && first + 1 < _lines->size()) {
                bytes -= (*_lines)[first].size();
                ++first;
            }
            _lines->erase(_lines->begin(), _lines->begin() + first);
            _bytes = bytes;
        }
    }

    std::vector<std::string>* _lines;
    const std::size_t _maxLines;
    const std::size_t _maxBytes;
    const std::size_t _maxLineBytes;
    std::size_t _bytes; // total size of *_lines
    std::string _pending; // the current line
    bool _cr; // the last character was a '\r'
    bool _cut; // characters were dropped from the current line
};

// Read sb until its end, into lines (on POSIX, sb is the pstreambuf reading the script's standard error).
// Reads what is available in chunks, rather than character by character or with std::getline (which would make a
// "line" of all the output that has no '\n').
inline void
readAll(std::streambuf* sb,
        LineCollector* lines)
{
    typedef std::streambuf::traits_type traits;
    char buf[4096];
    for (;;) {
        // blocks until there is something to read, or the end of file
        const traits::int_type c = sb->sbumpc();
        if (traits::eq_int_type(c, traits::eof())) {
            break;
        }
        buf[0] = traits::to_char_type(c);
        std::streamsize n = 1;
        // what is already buffered (or, for a pstreambuf, available in the pipe): reading it does not block
        const std::streamsize avail = sb->in_avail();
        if (avail > 0) {
            n += sb->sgetn(buf + 1, avail < (std::streamsize)sizeof(buf) - 1 ? avail : (std::streamsize)sizeof(buf) - 1);
        }
        lines->add(buf, (std::size_t)n);
    }
    lines->finish();
}

} // namespace RunScriptOutput

#endif // IO_RunScriptOutput_h
