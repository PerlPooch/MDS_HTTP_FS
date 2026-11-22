/*
 * Copyright (c) 2026 Marc D. Spencer.
 * SPDX-License-Identifier: MPL-2.0
 */

#include "MDS_HTTP_FS.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

#include <cstdio>
#include <cstring>
#include <utility>

namespace {

constexpr char stagingName[] = ".mds-http-upload.tmp";
constexpr char crudStagingName[] = ".mds-http-crud.tmp";
constexpr int missingArgument = -1;
constexpr int duplicateArgument = -2;

// LittleFS limits count bytes and reserve space for the terminating NUL.
#if defined(CONFIG_LITTLEFS_OBJ_NAME_LEN)
constexpr size_t maxNameBytes = CONFIG_LITTLEFS_OBJ_NAME_LEN - 1;
#elif defined(ARDUINO_ARCH_ESP8266)
constexpr size_t maxNameBytes = 31;
#else
constexpr size_t maxNameBytes = 63;
#endif

// Locate a unique decoded request argument without silently accepting duplicates.
int uniqueArgumentIndex(const MDS_HTTPServer& server, const char* name) {
	int found = missingArgument;
	for (int index = 0; index < server.args(); ++index) {
		if (server.argName(index) == name) {
			if (found != missingArgument) {
				return duplicateArgument;
			}
			found = index;
		}
	}

	return found;
}

// Check every component before passing a caller-supplied path to the filesystem.
bool validRelativePath(const String& path, bool hideStaging = true) {
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

		if (hideStaging && (length == sizeof(stagingName) - 1 || length == sizeof(crudStagingName) - 1)) {
			const String name = path.substring(componentStart, index);
			if (name.length() != length || name.equalsIgnoreCase(stagingName) || name.equalsIgnoreCase(crudStagingName)) {
				return false;
			}
		}

		componentStart = index + 1;
	}

	return true;
}

// Accept a normalized absolute filesystem root.
bool validRoot(const String& root) {
	return root.startsWith("/") && (root == "/" || validRelativePath(root.substring(1), false));
}

// Join a validated relative path to the root and report allocation failure.
bool joinPath(const String& root, const String& relative, String& result) {
	result = "";
	return result.concat(root) && (root == "/" || result.concat('/')) && result.concat(relative);
}

// Escape JSON delimiters in a validated filename before streaming the entry.
void sendEntryName(MDS_HTTPServer& server, const String& name) {
	size_t start = 0;
	for (size_t index = 0; index < name.length(); ++index) {
		const char character = name[index];
		if (character != '"' && character != '\\') {
			continue;
		}

		if (index > start) {
			server.sendContent(name.substring(start, index));
		}
		server.sendContent(character == '"' ? "\\\"" : "\\\\");
		start = index + 1;
	}
	if (start < name.length()) {
		server.sendContent(name.substring(start));
	}
}

// Serve a self-contained form that submits to the URL at which it was opened.
void sendFileUploadPage(MDS_HTTPServer& server, bool showKey) {
	static const char pageStart[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Upload</title>
<style>
	:root {
		--bg: #000;
		--text: #fff;
		--card: #20202080;
		--border: rgba(43, 14, 161, 0.58);
		--shadow: 0 12px 30px rgba(0, 0, 0, 0.10);
		--radius: 8px;
	}
	body {
		margin: 0;
		color: var(--text);
		background: var(--bg);
		font-family: Helvetica, Arial, sans-serif;
	}
	.card {
		box-sizing: border-box;
		width: calc(100% - 24px);
		max-width: 610px;
		margin: 10vh auto;
		padding: 24px;
		background: var(--card);
		border: 1px solid var(--border);
		border-radius: var(--radius);
		box-shadow: var(--shadow);
	}
	h1 { margin: 0 0 14px; font-size: 18px; font-weight: 600; }
	label { display: block; margin: 0 0 4px; }
	#destination-field, #upload-key-field { margin-top: 12px; }
	input, button { font: inherit; }
	input { box-sizing: border-box; max-width: 100%; }
	input[type=text], input[type=password] {
		width: 100%;
		padding: 6px 8px;
		color: var(--text);
		background: #141414;
		border: 1px solid #555;
		border-radius: var(--radius);
	}
	.actions { display: flex; justify-content: flex-end; margin-top: 12px; }
	button { border-radius: var(--radius); background: #fff; color: #000; cursor: pointer; }
	button:disabled { opacity: 0.65; cursor: default; }
	#status { margin: 12px 0 0; }
	#status:empty { margin: 0; }
</style>
</head>
<body>
<main class="card">
<h1>Upload File</h1>
<form method="post" enctype="multipart/form-data">
	<input id="file" name="file" type="file" required>
	<div id="destination-field" hidden>
		<label for="destination">Destination path</label>
		<input id="destination" type="text" placeholder="">
	</div>
)HTML";
	static const char keyField[] PROGMEM = R"HTML(
	<div id="upload-key-field" hidden>
		<label for="upload-key">Access key</label>
		<input id="upload-key" type="password" autocomplete="off" spellcheck="false">
	</div>
)HTML";
	static const char pageEnd[] PROGMEM = R"HTML(
	<div class="actions">
		<button type="submit">Upload</button>
	</div>
</form>
<p id="status" role="status"></p>
</main>
<script>
const form = document.querySelector('form');
const file = document.querySelector('#file');
const destination = document.querySelector('#destination');
const key = document.querySelector('#upload-key');
const button = form.querySelector('button');
const status = document.querySelector('#status');

document.querySelector('#destination-field').hidden = false;
if (key) key.parentElement.hidden = false;

file.addEventListener('change', () => {
	// Start with the selected filename so users can add a directory or rename it.
	destination.value = file.files[0]?.name ?? '';
});

form.addEventListener('submit', async (event) => {
	// Send the destination as the multipart filename understood by the backend.
	event.preventDefault();
	const selected = file.files[0];
	const filename = destination.value || selected.name;
	const body = new FormData();
	body.append('file', selected, filename);
	
	button.disabled = true;
	status.textContent = 'Uploading…';
	try {
	    const headers = key?.value ? { Authorization: 'Bearer ' + key.value } : {};
	    const response = await fetch(form.action, { method: 'POST', headers, body });
		if (!response.ok) {
			status.textContent = 'Upload failed. ' + await response.text();
		} else {
			status.textContent = 'Uploaded ' + filename + '.';
		}
	} catch {
		status.textContent = 'Could not reach the device. Try again.';
	} finally {
		button.disabled = false;
	}
});
</script>
</body>
</html>
)HTML";

	server.setContentLength(CONTENT_LENGTH_UNKNOWN);
	server.send_P(200, PSTR("text/html; charset=utf-8"), pageStart);
	if (showKey) {
		server.sendContent_P(keyField);
	}
	server.sendContent_P(pageEnd);
}

} // namespace

// Let callers opt into the browser UI while sharing the same multipart backend.
void MDS_HTTP_FS::addUpload(MDS_HTTP& http, const char* route, fs::FS& filesystem, const char* root,
	MDS_HTTP::BrowserForm form, AccessCheck access) {
	auto& server = http.server();
	if (form == MDS_HTTP::BrowserForm::Enabled) {
		const bool showKey = static_cast<bool>(access);
		server.on(route, HTTP_GET, [&server, showKey]() {
			// Serve the form before the browser has supplied its access key.
			sendFileUploadPage(server, showKey);
		});
	}

	auto* files = new MDS_HTTP_FS(filesystem, root);
	files->server_ = &server;
	files->access_ = std::move(access);
	http.addUpload(route, std::unique_ptr<MDS_HTTPUpload>(files));
}

// Share one filesystem backend across all methods at the application-selected route.
void MDS_HTTP_FS::addCrud(MDS_HTTP& http, const char* route, fs::FS& filesystem, const char* root,
	AccessCheck access, const char* corsOrigin, SpaceInfoProvider spaceInfo) {
	auto& server = http.server();
	auto* files = new MDS_HTTP_FS(filesystem, root);
	files->server_ = &server;
	files->crud_ = true;
	files->access_ = std::move(access);
	if (corsOrigin) {
		files->corsOrigin_ = corsOrigin;
	}

	// addUpload owns the backend for the lifetime of the registered callbacks.
	http.addUpload(route, std::unique_ptr<MDS_HTTPUpload>(files));
	server.on(route, HTTP_GET, [files]() {
		// Serve the collection or an individual file at the same route.
		files->handleGet();
	});
	server.on(route, HTTP_PUT, [files]() {
		// Create an empty directory without a multipart upload.
		files->handleMkdir();
	});
	server.on(route, HTTP_PATCH, [files]() {
		// Apply the same filesystem scope and access check to renames.
		files->handleRename();
	});
	server.on(route, HTTP_DELETE, [files]() {
		// Apply the same path and access rules to deletions.
		files->handleDelete();
	});

	files->addCorsPreflight(route, "GET, POST, PUT, PATCH, DELETE");

	if (spaceInfo) {
		String spaceRoute(route);
		if (spaceRoute.length() == std::strlen(route)
			&& spaceRoute.concat(spaceRoute.endsWith("/") ? "space" : "/space")) {
			server.on(spaceRoute.c_str(), HTTP_GET, [files, spaceInfo]() {
				// Read live space only after the file API authorizes the request.
				files->handleSpace(spaceInfo);
			});
			files->addCorsPreflight(spaceRoute.c_str(), "GET");
		}
	}
}

// An empty file is valid and can replace a previously nonempty asset.
MDS_HTTP_FS::MDS_HTTP_FS(fs::FS& filesystem, const char* root)
	: MDS_HTTPUpload("file", true), filesystem_(filesystem), root_(root) {
	if (root_.length() > 1 && root_.endsWith("/")) {
		root_.remove(root_.length() - 1);
	}
}

// Reserve a staging file in the configured root, away from published assets.
bool MDS_HTTP_FS::begin(const HTTPUpload& upload) {
	uploadStatus_ = 500;
	if (!canAccess()) {
		uploadStatus_ = 403;
		return false;
	}

	if (crud_) {
		const int error = resolvePath("path", destination_);
		if (error) {
			uploadStatus_ = error;
			return false;
		}
	} else if (!validRoot(root_) || !validRelativePath(upload.filename)
		|| !joinPath(root_, upload.filename, destination_)) {
		return false;
	}

	String stagingPath;
	if (!joinPath(root_, crud_ ? crudStagingName : stagingName, stagingPath)
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

	bool replacing = false;
	if (crud_) {
		// The multipart parser can add arguments after the first file starts.
		String path;
		const int error = resolvePath("path", path);
		if (error || path != destination_) {
			uploadStatus_ = error ? error : 400;
			return false;
		}

		auto existing = filesystem_.open(destination_.c_str(), "r");
		if (existing && existing.isDirectory()) {
			uploadStatus_ = 409;
			return false;
		}
		replacing = existing || filesystem_.exists(destination_.c_str());
	}

	// File::close() cannot report a buffered write failure on either Arduino core.
	auto staged = filesystem_.open(stagingPath_.c_str(), "r");
	const bool complete = staged && !staged.isDirectory() && staged.size() == written_;
	staged.close();

	if (!complete || !ensureParents(destination_)
		|| !filesystem_.rename(stagingPath_.c_str(), destination_.c_str())) {
		return false;
	}

	stagingPath_ = "";
	uploadStatus_ = replacing ? 200 : 201;
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

// Apply the application's policy uniformly across all filesystem API methods.
bool MDS_HTTP_FS::canAccess() const {
	return !access_ || access_(*server_);
}

// Resolve a single decoded path beneath the configured root.
int MDS_HTTP_FS::resolvePath(const char* parameter, String& path, bool allowRoot) const {
	if (!validRoot(root_)) {
		return 500;
	}

	const int found = uniqueArgumentIndex(*server_, parameter);
	if (found == duplicateArgument) {
		return 400;
	}
	if (found == missingArgument) {
		if (!allowRoot) {
			return 400;
		}
		return path.concat(root_) ? 0 : 500;
	}

	const String relative = server_->arg(found);
	if (!validRelativePath(relative)) {
		return 400;
	}
	return joinPath(root_, relative, path) ? 0 : 500;
}

// Return a concise HTTP error without exposing internal filesystem details.
static void sendCrudError(MDS_HTTPServer& server, int status) {
	const char* message = "Filesystem operation failed\n";
	switch (status) {
		case 400: message = "Invalid filesystem request\n"; break;
		case 403: message = "Access denied\n"; break;
		case 404: message = "Path not found\n"; break;
		case 409: message = "Path has the wrong type\n"; break;
	}
	server.send(status, "text/plain", message);
}

// Answer preflight before the browser can send its bearer token.
void MDS_HTTP_FS::addCorsPreflight(const char* route, const char* methods) {
	if (!corsOrigin_.length()) {
		return;
	}

	server_->on(route, HTTP_OPTIONS, [this, methods]() {
		// Advertise only the methods supported by this route.
		auto& server = *server_;
		sendCorsHeader(server);
		server.sendHeader("Access-Control-Allow-Methods", methods);
		server.sendHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
		server.send(204, "text/plain", "");
	});
}

// Use the registration-time origin for both preflight and actual responses.
void MDS_HTTP_FS::sendCorsHeader(MDS_HTTPServer& server) const {
	if (corsOrigin_.length()) {
		server.sendHeader("Access-Control-Allow-Origin", corsOrigin_.c_str());
	}
}

// Let API clients distinguish invalid requests, failed writes, creation and replacement.
void MDS_HTTP_FS::sendResponse(MDS_HTTPServer& server, MDS_HTTP::Error error, const char* message) {
	sendCorsHeader(server);

	using Error = MDS_HTTP::Error;
	const bool incomplete = error == Error::InvalidUpload || error == Error::UploadAborted;
	if ((error == Error::UploadStartFailed && uploadStatus_ == 403)
		|| (incomplete && !canAccess())) {
		server.sendHeader("Cache-Control", "no-store");
		sendCrudError(server, 403);
		return;
	}
	if (!crud_) {
		MDS_HTTPUpload::sendResponse(server, error, message);
		return;
	}

	server.sendHeader("Cache-Control", "no-store");
	switch (error) {
		case Error::None:
			server.send(uploadStatus_, "text/plain", message);
			break;
		case Error::InvalidUpload:
		case Error::UploadAborted:
			sendCrudError(server, 400);
			break;
		case Error::UploadStartFailed:
		case Error::UploadFinalizeFailed:
			sendCrudError(server, uploadStatus_);
			break;
		default:
			sendCrudError(server, 500);
			break;
	}
}

// Return a small, uncached snapshot from the application-supplied space provider.
void MDS_HTTP_FS::handleSpace(const SpaceInfoProvider& spaceInfo) {
	auto& server = *server_;
	sendCorsHeader(server);
	server.sendHeader("Cache-Control", "no-store");
	if (!canAccess()) {
		sendCrudError(server, 403);
		return;
	}

	size_t totalBytes = 0;
	size_t usedBytes = 0;
	if (!spaceInfo(totalBytes, usedBytes) || !totalBytes || usedBytes > totalBytes) {
		server.send(503, "text/plain", "Filesystem space is unavailable\n");
		return;
	}

	char response[80];
	std::snprintf(response, sizeof(response), "{\"totalBytes\":%lu,\"usedBytes\":%lu}\n",
		static_cast<unsigned long>(totalBytes), static_cast<unsigned long>(usedBytes));
	server.send(200, "application/json", response);
}

// Stream a requested file or list a directory when no file path was specified.
void MDS_HTTP_FS::handleGet() {
	auto& server = *server_;
	sendCorsHeader(server);
	server.sendHeader("Cache-Control", "no-store");
	if (!canAccess()) {
		sendCrudError(server, 403);
		return;
	}

	bool download = false;
	bool directory = false;
	for (int index = 0; index < server.args(); ++index) {
		const String name = server.argName(index);
		download = download || name == "path";
		directory = directory || name == "dir";
	}
	if (download && directory) {
		sendCrudError(server, 400);
		return;
	}

	String path;
	const int error = resolvePath(download ? "path" : "dir", path, !download);
	if (error) {
		sendCrudError(server, error);
		return;
	}
	if (!download) {
		listDirectory(path);
		return;
	}

	auto file = filesystem_.open(path.c_str(), "r");
	if (!file) {
		sendCrudError(server, filesystem_.exists(path.c_str()) ? 500 : 404);
	} else if (file.isDirectory()) {
		sendCrudError(server, 409);
	} else {
		server.streamFile(file, "application/octet-stream");
	}
}

// Enumerate one level of the filesystem without buffering the entire listing.
void MDS_HTTP_FS::listDirectory(const String& path) {
	auto& server = *server_;
	auto directory = filesystem_.open(path.c_str(), "r");
	if (!directory) {
		if (filesystem_.exists(path.c_str())) {
			sendCrudError(server, 500);
		} else if (path == root_) {
			// An unused upload root is created lazily when the first file is written.
			server.send(200, "application/json", "[]\n");
		} else {
			sendCrudError(server, 404);
		}
		return;
	}
	if (!directory.isDirectory()) {
		sendCrudError(server, 409);
		return;
	}

	server.setContentLength(CONTENT_LENGTH_UNKNOWN);
	server.send(200, "application/json", "");
	server.sendContent("[");
	bool first = true;
	while (auto entry = directory.openNextFile()) {
		const String name = entry.name();
		if (!validRelativePath(name)) {
			continue;
		}

		server.sendContent(first ? "{\"name\":\"" : ",{\"name\":\"");
		sendEntryName(server, name);
		if (entry.isDirectory()) {
			server.sendContent("\",\"type\":\"directory\"}");
		} else {
			server.sendContent("\",\"type\":\"file\",\"size\":");
			server.sendContent(String(static_cast<unsigned long>(entry.size())));
			server.sendContent("}");
		}
		first = false;
	}
	server.sendContent("]\n");
}

// Rename or move an existing entry with one filesystem operation.
void MDS_HTTP_FS::handleRename() {
	auto& server = *server_;
	sendCorsHeader(server);
	server.sendHeader("Cache-Control", "no-store");
	if (!canAccess()) {
		sendCrudError(server, 403);
		return;
	}

	String source;
	const int pathError = resolvePath("path", source);
	if (pathError) {
		sendCrudError(server, pathError);
		return;
	}

	const int nameIndex = uniqueArgumentIndex(server, "name");
	const int toIndex = uniqueArgumentIndex(server, "to");
	if (nameIndex == duplicateArgument || toIndex == duplicateArgument
		|| (nameIndex >= 0) == (toIndex >= 0)) {
		server.send(400, "text/plain", "Supply exactly one name or destination path\n");
		return;
	}

	String destination;
	if (toIndex >= 0) {
		const int destinationError = resolvePath("to", destination);
		if (destinationError) {
			sendCrudError(server, destinationError);
			return;
		}
	} else {
		const String name = server.arg(nameIndex);
		if (name.indexOf('/') >= 0 || !validRelativePath(name)) {
			server.send(400, "text/plain", "Invalid new name\n");
			return;
		}
		const auto parentLength = static_cast<size_t>(std::strrchr(source.c_str(), '/') - source.c_str() + 1);
		destination = source.substring(0, parentLength);
		if (destination.length() != parentLength || !destination.concat(name)) {
			sendCrudError(server, 500);
			return;
		}
	}

	const char* separator = std::strrchr(destination.c_str(), '/');
	if (std::strlen(separator + 1) > maxNameBytes) {
		server.send(400, "text/plain", "New name exceeds the filesystem name limit\n");
		return;
	}

	if (!filesystem_.exists(source.c_str())) {
		sendCrudError(server, 404);
		return;
	}
	if (source != destination) {
		if (destination.startsWith(source.c_str()) && destination[source.length()] == '/') {
			server.send(409, "text/plain", "An item cannot be moved inside itself\n");
			return;
		}
		if (toIndex >= 0) {
			const auto parentEnd = static_cast<size_t>(separator - destination.c_str());
			const size_t parentLength = parentEnd ? parentEnd : 1;
			const String parentPath = destination.substring(0, parentLength);
			if (parentPath.length() != parentLength) {
				sendCrudError(server, 500);
				return;
			}
			auto parent = filesystem_.open(parentPath.c_str(), "r");
			if (!parent) {
				if (filesystem_.exists(parentPath.c_str())) {
					sendCrudError(server, 500);
				} else {
					server.send(404, "text/plain", "Destination folder does not exist\n");
				}
				return;
			}
			if (!parent.isDirectory()) {
				server.send(409, "text/plain", "Destination parent is not a folder\n");
				return;
			}
			parent.close();
		}
		if (filesystem_.exists(destination.c_str())) {
			server.send(409, "text/plain", "An item with that name already exists\n");
			return;
		}
		if (!filesystem_.rename(source.c_str(), destination.c_str())) {
			sendCrudError(server, 500);
			return;
		}
	}

	server.send(204, "text/plain", "");
}

// Create missing parents in order and distinguish an existing folder from a new one.
int MDS_HTTP_FS::createDirectoryTree(const String& path) {
	for (size_t start = 1;;) {
		const int separator = path.indexOf('/', start);
		const size_t end = separator < 0 ? path.length() : static_cast<size_t>(separator);
		const String component = path.substring(0, end);
		if (component.length() != end) {
			return 500;
		}

		auto entry = filesystem_.open(component.c_str(), "r");
		const bool created = !entry && filesystem_.mkdir(component.c_str());
		if (!created) {
			if (!entry) {
				// A directory can appear between the initial lookup and mkdir.
				entry = filesystem_.open(component.c_str(), "r");
			}
			if (!entry) {
				return 500;
			}
			if (!entry.isDirectory()) {
				return 409;
			}
			entry.close();
		}

		if (separator < 0) {
			return created ? 201 : 200;
		}
		start = end + 1;
	}
}

// Create an empty directory without requiring a file to be uploaded.
void MDS_HTTP_FS::handleMkdir() {
	auto& server = *server_;
	sendCorsHeader(server);
	server.sendHeader("Cache-Control", "no-store");
	if (!canAccess()) {
		sendCrudError(server, 403);
		return;
	}
	if (uniqueArgumentIndex(server, "path") != missingArgument) {
		sendCrudError(server, 400);
		return;
	}

	String path;
	const int error = resolvePath("dir", path);
	if (error) {
		sendCrudError(server, error);
		return;
	}

	// Validate the whole relative path before creating even the first directory.
	size_t start = root_.length() + (root_ == "/" ? 0 : 1);
	for (size_t index = start; index <= path.length(); ++index) {
		if (index == path.length() || path[index] == '/') {
			if (index - start > maxNameBytes) {
				server.send(400, "text/plain", "Folder name exceeds the filesystem name limit\n");
				return;
			}
			start = index + 1;
		}
	}

	const int status = createDirectoryTree(path);
	if (status == 409) {
		server.send(409, "text/plain", "A file occupies the folder path\n");
	} else if (status == 200 || status == 201) {
		server.send(status, "text/plain", "");
	} else {
		sendCrudError(server, status);
	}
}

// Delete a regular file or empty directory identified by the request.
void MDS_HTTP_FS::handleDelete() {
	auto& server = *server_;
	sendCorsHeader(server);
	server.sendHeader("Cache-Control", "no-store");
	if (!canAccess()) {
		sendCrudError(server, 403);
		return;
	}

	String path;
	const int error = resolvePath("path", path);
	if (error) {
		sendCrudError(server, error);
		return;
	}
	auto file = filesystem_.open(path.c_str(), "r");
	if (!file) {
		sendCrudError(server, filesystem_.exists(path.c_str()) ? 500 : 404);
		return;
	}
	const bool directory = file.isDirectory();
	if (directory) {
		while (auto child = file.openNextFile()) {
			const String name = child.name();
			child.close();
			if (name != "." && name != "..") {
				file.close();
				server.send(409, "text/plain", "Folder is not empty\n");
				return;
			}
		}
	}
	file.close();

	// rmdir also refuses a directory that gains a child after the check above.
	const bool removed = directory ? filesystem_.rmdir(path.c_str()) : filesystem_.remove(path.c_str());
	if (!removed) {
		sendCrudError(server, 500);
		return;
	}
	server.send(204, "text/plain", "");
}
