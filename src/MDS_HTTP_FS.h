/*
 * Copyright (c) 2026 Marc D. Spencer.
 * SPDX-License-Identifier: MPL-2.0
 */

#pragma once

#include <FS.h>
#include <MDS_HTTPUpload.h>

// Receive a single file beneath a caller-selected directory on a mounted filesystem.
class MDS_HTTP_FS final : public MDS_HTTPUpload {
public:
	enum class BrowserForm {
		Disabled,
		Enabled
	};

	// Install the multipart POST route and optionally serve its browser form at the same URL.
	static void install(MDS_HTTP& http, const char* route, fs::FS& filesystem, const char* root,
		BrowserForm form = BrowserForm::Disabled);

	// Borrow the filesystem and copy the absolute upload root.
	MDS_HTTP_FS(fs::FS& filesystem, const char* root);

private:
	// Open a staging file after checking the multipart filename and upload root.
	bool begin(const HTTPUpload& upload) override;

	// Stream a complete chunk and track the expected file size.
	bool write(uint8_t* data, size_t size) override;

	// Publish the staged file after checking that the writes reached storage.
	bool commit() override;

	// Close and remove the staging file after an interrupted or failed upload.
	void abort() override;

	// Create missing parent directories from the root down.
	bool ensureParents(const String& path);

	fs::FS& filesystem_;
	String root_;
	String destination_;
	String stagingPath_;
	fs::File stagingFile_;
	size_t written_ = 0;
};
