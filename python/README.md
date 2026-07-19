# starweb

STWP client and server for [StarWeb](https://github.com/MIXIDtheSilly/StarWeb) —
a pure-Python implementation of the `moon://` (plaintext) and `star://` (TLS)
schemes. No dependencies beyond the standard library.

## Install

```sh
pip install starweb
```

## Client

```python
import starweb

res = starweb.get("star://localhost:8490/api/time")
print(res.status_code, res.body)
```

Sessions reuse connections and let you pin a CA:

```python
with starweb.Session(cafile="certs/localhost.pem") as s:
    res = s.request("POST", "star://localhost:8490/api/echo")
    print(res.tls.version, res.tls.cipher, res.tls.alpn)
```

## Server

```python
from starweb import App, Response

app = App()


@app.route("/api/greet/<name>")
def greet(req, name):
    return {"hello": name}


@app.route("/api/echo", methods=["POST"])
def echo(req):
    return Response(200, body=req.body)


app.mount_static("/", "www")

if __name__ == "__main__":
    app.run(scheme="both", port=8090, tls_port=8490,
            cert="certs/localhost.pem", key="certs/localhost.key")
```

Routes take precedence over mounted static files, so `/api/*` still reaches the
handlers above.

## CLI

```sh
starweb get star://localhost:8490/api/time -v
starweb serve examples/api.py --scheme both --log
```

`starweb get` writes the response body to stdout and headers to stderr under
`-v`. `starweb serve` loads a Python file and runs the first `App` instance it
finds.

## License

MIT — see [LICENSE](LICENSE).
