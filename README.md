# WebLanguageServer

A simple HTTP server that extracts language information from HTML documents using a plugin architecture.

## Building

```bash
make
```

## Configuration

See source code for configuration options.

## Usage

Start the server and send HTML content to the `/extract` endpoint:

```bash
curl -X POST --data-binary @page.html http://localhost:5000/extract
```
