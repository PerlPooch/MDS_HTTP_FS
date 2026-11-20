/*
 * Copyright (c) 2026 Marc D. Spencer.
 * SPDX-License-Identifier: MPL-2.0
 */

#include "MDS_HTTP_FS.h"

#include <utility>

namespace {

constexpr char stagingName[] = ".mds-http-upload.tmp";

// Check every component before passing a caller-supplied path to the filesystem.
bool validRelativePath(const String& path) {
	size_t componentStart = 0;

	for (size_t index = 0; index <= path.length(); ++index) {
		if (index < path.length() && path[index] != '/') {
			const auto character = static_cast<unsigned char>(path[index]);
			if (character < 0x20 || character == 0x7f || character == '\\') {
				return false;
			}
			continue;
		}

		const size_t length = index - componentStart;
		if (length == 0) {
			return false;
		}

		const char* component = path.c_str() + componentStart;
		if ((length == 1 && component[0] == '.') || (length == 2 && component[0] == '.' && component[1] == '.')) {
			return false;
		}

		componentStart = index + 1;
	}

	return true;
}

} // namespace

// An empty file is valid and can replace a previously nonempty asset.
MDS_HTTP_FS::MDS_HTTP_FS(fs::FS& filesystem, const char* root)
	: MDS_HTTPUpload("file", true), filesystem_(filesystem), root_(root) {
	if (root_.length() > 1 && root_.endsWith("/")) {
		root_.remove(root_.length() - 1);
	}
}

// Reserve a staging file in the configured root, away from published assets.
bool MDS_HTTP_FS::begin(const HTTPUpload& upload) {
	const String& filename = upload.filename;
	if (!root_.startsWith("/") || !validRelativePath(filename)
		|| (root_ != "/" && !validRelativePath(root_.substring(1)))) {
		return false;
	}

	const int separator = filename.indexOf('/');
	const size_t firstLength = separator < 0 ? filename.length() : static_cast<size_t>(separator);
	const String firstComponent = filename.substring(0, firstLength);
	if (firstComponent.length() != firstLength || firstComponent.equalsIgnoreCase(stagingName)) {
		return false;
	}

	destination_ = "";
	if (!destination_.concat(root_) || (root_ != "/" && !destination_.concat('/'))
		|| !destination_.concat(filename)) {
		return false;
	}

	String stagingPath;
	if (!stagingPath.concat(root_) || (root_ != "/" && !stagingPath.concat('/'))
		|| !stagingPath.concat(stagingName)
		|| !ensureParents(stagingPath)) {
		return false;
	}

	stagingPath_ = std::move(stagingPath);
	stagingFile_ = filesystem_.open(stagingPath_.c_str(), "w");
	if (!stagingFile_) {
		abort();
		return false;
	}

	written_ = 0;
	return true;
}

// Treat a short write as a failure and leave cleanup to the upload handler.
bool MDS_HTTP_FS::write(uint8_t* data, size_t size) {
	if (stagingFile_.write(data, size) != size) {
		return false;
	}

	written_ += size;
	return true;
}

// Use the filesystem's replace-by-rename operation without deleting the destination first.
bool MDS_HTTP_FS::commit() {
	stagingFile_.close();

	// File::close() cannot report a buffered write failure on either Arduino core.
	auto staged = filesystem_.open(stagingPath_.c_str(), "r");
	const bool complete = staged && !staged.isDirectory() && staged.size() == written_;
	staged.close();

	if (!complete || !ensureParents(destination_)
		|| !filesystem_.rename(stagingPath_.c_str(), destination_.c_str())) {
		return false;
	}

	stagingPath_ = "";
	return true;
}

// Remove only the reserved staging file, leaving published files available.
void MDS_HTTP_FS::abort() {
	stagingFile_.close();

	if (stagingPath_.length() != 0) {
		filesystem_.remove(stagingPath_.c_str());
		stagingPath_ = "";
	}
}

// Create each missing directory so uploads can introduce nested asset paths.
bool MDS_HTTP_FS::ensureParents(const String& path) {
	for (int separator = path.indexOf('/', 1); separator >= 0;
		separator = path.indexOf('/', separator + 1)) {
		const String directory = path.substring(0, separator);
		if (directory.length() != static_cast<size_t>(separator)
			|| (!filesystem_.exists(directory.c_str()) && !filesystem_.mkdir(directory.c_str()))) {
			return false;
		}
	}

	return true;
}
