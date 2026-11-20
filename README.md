# MDS_HTTP_FS

A filesystem upload backend for `MDS_HTTP` on ESP32 and ESP8266. It takes a mounted
Arduino `fs::FS` and uses the filesystem's same-volume rename operation to replace
files. LittleFS supports this operation.

Register a unique route before starting the listener. This requires the version
of `MDS_HTTP` with `addUpload()`.

```cpp
#include <LittleFS.h>
#include <MDS_HTTP_FS.h>

// Register a POST route for file uploads into the website directory.
void registerWebsiteUpload(MDS_HTTP& http) {
	http.addUpload("/upload-file",
		std::unique_ptr<MDS_HTTPUpload>(new MDS_HTTP_FS(LittleFS, "/www")));
}
```

Send one multipart field named `file`. The multipart filename is relative to the
upload root. For example, this writes `/www/assets/style.css`, creating missing
parent directories.

```sh
curl --fail-with-body -F 'file=@style.css;filename=assets/style.css' http://BOARD_IP/upload-file
```

The backend accepts empty files. It rejects absolute filenames, empty path
components, dot segments, backslashes, and control characters. The root reserves
`.mds-http-upload.tmp` for staging. Use one uploader per root and keep the filesystem
mounted for the duration of each request. If a transfer is interrupted, the previous
file stays in place. A staging file left by a reset is reused on the next upload.
