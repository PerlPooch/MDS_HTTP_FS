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
	// Return false to deny the request. The application supplies its access policy.
	using AccessCheck = std::function<bool(const MDS_HTTPServer&)>;

	// Report the mounted volume's total and used bytes, or return false if unavailable.
	using SpaceInfoProvider = std::function<bool(size_t& totalBytes, size_t& usedBytes)>;

	// Register file operations and directory creation at a single route before http.begin().
	static void addCrud(MDS_HTTP& http, const char* route, fs::FS& filesystem, const char* root,
		AccessCheck access = nullptr, const char* corsOrigin = nullptr, SpaceInfoProvider spaceInfo = nullptr);

	// Register file POST and optionally a GET form at the same route before http.begin().
	static void addUpload(MDS_HTTP& http, const char* route, fs::FS& filesystem, const char* root,
		MDS_HTTP::BrowserForm form = MDS_HTTP::BrowserForm::Disabled, AccessCheck access = nullptr);

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

	// Return the route-specific upload result without changing the browser upload contract.
	void sendResponse(MDS_HTTPServer& server, MDS_HTTP::Error error, const char* message) override;

	// Register OPTIONS for a route when cross-origin access is configured.
	void addCorsPreflight(const char* route, const char* methods);
	// Add the configured origin to a CRUD response.
	void sendCorsHeader(MDS_HTTPServer& server) const;

	// List a directory or stream a file based on the request parameters.
	void handleGet();
	// Read and return the capacity of the entire filesystem backing this route.
	void handleSpace(const SpaceInfoProvider& spaceInfo);
	// Remove one regular file or empty directory within the configured root.
	void handleDelete();
	// Rename or move one file or directory without replacing another entry.
	void handleRename();
	// Create a directory from the root-relative dir parameter.
	void handleMkdir();
	// Create missing directory components and report the outcome for the final directory.
	int createDirectoryTree(const String& path);
	// Stream a JSON array containing the immediate children of a directory.
	void listDirectory(const String& path);
	// Check the application policy before any filesystem operation.
	bool canAccess() const;
	// Resolve a unique root-relative path parameter and return an HTTP error code on failure.
	int resolvePath(const char* parameter, String& path, bool allowRoot = false) const;

	// Create missing parent directories from the root down.
	bool ensureParents(const String& path);

	fs::FS& filesystem_;
	String root_;
	String destination_;
	String stagingPath_;
	fs::File stagingFile_;
	size_t written_ = 0;
	MDS_HTTPServer* server_ = nullptr;
	bool crud_ = false;
	AccessCheck access_;
	String corsOrigin_;
	int uploadStatus_ = 500;
};
