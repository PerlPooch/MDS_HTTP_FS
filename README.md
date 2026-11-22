# MDS_HTTP_FS

`MDS_HTTP_FS` adds file management routes to `MDS_HTTP` on ESP32 and ESP8266.
It uses a caller-mounted Arduino `fs::FS`. Use LittleFS or a filesystem that
supports replacing a file with a same-volume rename.

## Register routes

Choose the HTTP route and the absolute filesystem root separately. Register each
route once, before `http.begin()` and before any catch-all static handler.
Keep the filesystem mounted while the routes are in use.

```cpp
#include <LittleFS.h>
#include <MDS_HTTP_FS.h>

// Register the file API and the optional browser uploader.
void registerWebsiteFiles(MDS_HTTP& http) {
	MDS_HTTP_FS::addCrud(http, "/api/files", LittleFS, "/www");
	MDS_HTTP_FS::addUpload(http, "/upload-file", LittleFS, "/www",
		MDS_HTTP::BrowserForm::Enabled);
}
```

You can register either route independently. The examples below use `/api/files`
and `/upload-file` as the application-selected routes.

## File API

For a browser app served from a different origin, pass the allowed origin as
the argument after the access check in `addCrud()`. Pass `"*"` to allow any origin.

```cpp
MDS_HTTP_FS::addCrud(http, "/api/files", LittleFS, "/www", nullptr, "*");
```

Omit the origin or pass an empty string to leave CORS disabled. A configured
origin enables browser preflight for GET, POST, PUT, PATCH and DELETE,
including requests with an `Authorization` header. Any access check supplied to `addCrud()`
still applies to the file operations.

| Request | Success | Response |
| --- | --- | --- |
| `GET /api/files` | `200` | JSON listing of the root's immediate children |
| `GET /api/files?dir=assets` | `200` | JSON listing of that directory's immediate children |
| `GET /api/files/space` | `200` | JSON with the mounted volume's `totalBytes` and `usedBytes` when configured |
| `GET /api/files?path=assets/style.css` | `200` | File bytes with content type `application/octet-stream` |
| `POST /api/files?path=assets/style.css` | `201` for creation, `200` for replacement | Plain-text confirmation |
| `PUT /api/files?dir=assets/icons` | `201` if created, `200` if already a directory | Empty response |
| `PATCH /api/files?path=assets/style.css&name=main.css` | `204` | Empty response after renaming |
| `PATCH /api/files?path=assets/style.css&to=styles/style.css` | `204` | Empty response after moving |
| `DELETE /api/files?path=assets/style.css` | `204` | Empty response after deleting a file or empty directory |

Pass a `SpaceInfoProvider` as the last argument to `addCrud()` to register
the `/space` subroute. The callback fills the total and used byte counts for
the filesystem containing the configured root. A failed callback or invalid
counts returns `503`. The space route uses the file API's access check and CORS
configuration.

Listings are JSON arrays. Each entry has a basename in `name` and a `type` of
`directory` or `file`. File entries also report `size` in bytes. Ordering follows
the filesystem. A root that has yet to be created lists as `[]`.

```json
[
  {"name": "assets", "type": "directory"},
  {"name": "index.html", "type": "file", "size": 1485}
]
```

Use a URL-encoded, root-relative `path` or `dir`. Omit both to list the root.
A GET accepts at most one of them. POST, PATCH and DELETE require `path`. PUT
takes a single `dir`. No request body is needed.

- POST takes one multipart file field named `file`. The query `path` selects the
  destination. Missing parent directories are created when needed.
- A successful POST publishes the complete upload by rename. Empty files are
  valid. Failed or interrupted uploads preserve the previous file.
- PUT creates a directory and any missing parents. It returns `200` if the
  destination already exists as a directory. A regular file anywhere along the
  path returns `409`. Every new path component uses the same byte limit as a
  PATCH destination basename. A retry can finish creating missing folders
  after a storage error.
- DELETE removes a regular file or empty directory. A non-empty directory
  returns `409` with its contents intact.
- PATCH takes exactly one `name` or `to` argument in addition to `path`. Give
  `name` a new basename to rename within the parent, or give `to` the full
  root-relative destination path to move an entry. The destination parent must
  already exist. An occupied destination or a move into the source's own
  subtree returns `409` and preserves the existing entries. An unchanged path
  succeeds.
  The destination basename follows the rules below and allows up to 31 bytes
  on ESP8266 or 63 bytes by default on ESP32. An ESP32 build with a different
  `CONFIG_LITTLEFS_OBJ_NAME_LEN` uses that value minus the terminating byte.

| Error | Meaning |
| --- | --- |
| `400` | Invalid, empty, duplicate, or conflicting path parameters, an invalid new name, or an invalid multipart upload |
| `403` | The application's access check denied the request |
| `404` | The requested file or directory is missing |
| `409` | The request expects a file where a directory exists, a directory where a file exists, the rename destination is occupied, or the directory to delete is non-empty |
| `500` | A filesystem operation failed or the configured root is invalid |
| `503` | Filesystem capacity is unavailable |

Error bodies are plain text. Paths reject leading slashes, empty components,
`.` and `..` components, backslashes, and control characters. Both
`.mds-http-upload.tmp` and `.mds-http-crud.tmp` are reserved names.

### Try the API with curl

This sequence creates and deletes a uniquely named test file under the root.
The second POST demonstrates replacement.

```sh
BASE=http://BOARD_IP
PROBE="curl-test-$(date +%s)-$$.txt"

curl -si "$BASE/api/files"
printf 'first\n' | curl -si -F 'file=@-;filename=probe.txt' "$BASE/api/files?path=$PROBE"
curl -si "$BASE/api/files?path=$PROBE"
printf 'second\n' | curl -si -F 'file=@-;filename=probe.txt' "$BASE/api/files?path=$PROBE"
curl -si -X DELETE "$BASE/api/files?path=$PROBE"
```

## Standalone upload

`addUpload()` registers a multipart POST. Set `MDS_HTTP::BrowserForm::Enabled` to
serve the browser form at that URL with GET. The browser lets users enter a
root-relative destination path. Direct multipart clients set the destination in
the filename of the `file` part.

```sh
curl --fail-with-body -F 'file=@style.css;filename=assets/style.css' http://BOARD_IP/upload-file
```

## Application access checks

`MDS_HTTP_FS::AccessCheck` is a
`std::function<bool(const MDS_HTTPServer&)>` supplied by the application at
registration. It runs on file API requests and before the standalone uploader
stages a file. Returning `false` returns `403`. An omitted check leaves the
route open. Use the same check for both registrations when both manage the
same files.

```cpp
MDS_HTTP_FS::addCrud(http, "/api/files", LittleFS, "/www", check);
MDS_HTTP_FS::addUpload(http, "/upload-file", LittleFS, "/www",
	MDS_HTTP::BrowserForm::Enabled, check);
```

The browser form is public so users can enter a key. When an access check is
bound, the form includes an optional password field and sends its value in
`Authorization: Bearer <key>` on POST. The application defines how to validate
the credential and where to store it.

## Arduino examples

- [BrowserUpload](examples/BrowserUpload/BrowserUpload.ino) prints the URL of
  the upload form.
- [PostOnlyUpload](examples/PostOnlyUpload/PostOnlyUpload.ino) prints a curl
  command to upload a local `hello.txt`.

Both sketches write under `/uploads` on LittleFS. Set `WifiSsid` and
`WifiPassword`, select a board with a filesystem partition, and open the Serial
Monitor at 115200 baud. To initialize a blank partition, set
`FormatOnMountFailure` to `true` for the first run. This erases the filesystem if
mounting fails.
