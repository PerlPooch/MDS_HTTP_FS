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

// Serve a self-contained form that submits to the URL at which it was opened.
void sendFileUploadPage(MDS_HTTPServer& server) {
	static const char page[] PROGMEM = R"HTML(<!doctype html>
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
	#destination-field { margin-top: 12px; }
	input, button { font: inherit; }
	input { box-sizing: border-box; max-width: 100%; }
	input[type=text] {
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
const button = form.querySelector('button');
const status = document.querySelector('#status');

document.querySelector('#destination-field').hidden = false;

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
	    const response = await fetch(form.action, { method: 'POST', body });
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

	server.send_P(200, PSTR("text/html; charset=utf-8"), page);
}

} // namespace

// Let callers opt into the browser UI while sharing the same multipart backend.
void MDS_HTTP_FS::addUpload(MDS_HTTP& http, const char* route, fs::FS& filesystem, const char* root,
	MDS_HTTP::BrowserForm form) {
	if (form == MDS_HTTP::BrowserForm::Enabled) {
		auto& server = http.server();
		server.on(route, HTTP_GET, [&server]() {
			// Keep HTML presentation local to the filesystem feature.
			sendFileUploadPage(server);
		});
	}

	http.addUpload(route, std::unique_ptr<MDS_HTTPUpload>(new MDS_HTTP_FS(filesystem, root)));
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
