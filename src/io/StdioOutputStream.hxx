// SPDX-License-Identifier: BSD-2-Clause
// author: Max Kellermann <max.kellermann@gmail.com>

#ifndef STDIO_OUTPUT_STREAM_HXX
#define STDIO_OUTPUT_STREAM_HXX

#include "OutputStream.hxx"

#include <stdio.h>

class StdioOutputStream final : public OutputStream {
	FILE *const file;

public:
	explicit StdioOutputStream(FILE *_file) noexcept:file(_file) {}

	/* virtual methods from class OutputStream */
	void Write(const void *data, size_t size) override {
		fwrite(data, 1, size, file);

		/* this class is debug-only and ignores errors */
	}
};

#endif
