# Red

Pila IPv4 propia. Un NIC bound. Un TCP. HTTP/1.0. HTTPS con SNI.

Por defecto **WWW está OFF**. Sin `www on` no hay DHCP, no hay DNS,
no hay curl. `ipconf` igual te enseña la MAC.

## Encender

```text
www on
ipconf
www status
```

QEMU: `10.0.2.15/24` gw `10.0.2.2` dns `10.0.2.3` si DHCP no llega.
Metal: DHCP 4 s; si falla, `www config <ip> <mask> <gw> [dns]`.

Drivers: virtio-net, Intel e1000, Realtek 810/8168 (`r8169`), RTL8139.
`nics` lista **todo** el PCI de red, incluso chips que no conduzco
(WiFi RTL8188EE: no). `ipconf` es el bound + IP + MAC.

## `ipconf`

```text
ipconf
ipconfig     # alias
ifconfig     # alias
```

Ejemplo QEMU:

```text
bound:
  iface0  virtio-net
    state    up
    ether    52:54:00:12:34:56
    inet     10.0.2.15
    netmask  255.255.255.0
    gateway  10.0.2.2
    dns      10.0.2.3
    www      ON
```

## `curl`

```text
curl https://example.com
curl -I -L http://example.com
curl -X POST -d "usuario=juan&clave=123" https://ejemplo.com
curl -o /tmp/page.html https://example.com
curl -O https://example.com/index.html
curl -b "sid=1" -c /tmp/jar https://ejemplo.com
curl -dom https://www.rogexlaboratories.com
curl search <query>
```

| Flag | Qué |
|---|---|
| `-X` `--request` | GET POST HEAD PUT |
| `-d` `--data` | cuerpo POST (`x-www-form-urlencoded`) |
| `-I` `--head` | solo cabeceras |
| `-L` `--location` | seguir 3xx (off por defecto, como curl de verdad) |
| `-O` | basename de la URL → `/tmp/` |
| `-o` | path |
| `-b` | `Cookie:` |
| `-c` | jar (`Set-Cookie`) |
| `-H` | cabecera extra |
| `--html` | crudo |
| `-dom` | espera ~2 s, re-GET, texto visible + HTML |

Comillas `"…"` y `'…'` funcionan. Si escribes `usuario=juan$clave=123`
(el `$` que la gente pone cuando quiere `&`), lo convierto a `&` cuando
parece un form. El shell de echOS **no** expande `$VAR`.

`-dom` **no ejecuta JS**. No hay Vite en el unikernel. Espera y vuelve
a pedir el documento. Una SPA te va a dar el index vacío más una nota.
Si quieres el HTML, `-o` guarda hasta el tope de RXFS (64 KiB).

User-Agent: `EchOS-nova/2.0 (echOS)`.

## `wdl` / `wget`

Download a fichero. Default `/tmp/<basename>`. Sigue redirects.

```text
wdl https://example.com
wdl -O /users/live/Downloads/index.html https://example.com
wdl -c -O /tmp/big.bin https://example.com/big.bin
wdl -b <url>
```

| Flag | wget/wdl | curl |
|---|---|---|
| `-O` / `-o` | destino | destino |
| `-c` | **continue** (`Range: bytes=N-`) | **cookie-jar** |
| `-b` | “background” (no hay fork: corre igual) | **Cookie:** |
| `-L` | follow (ya es default) | follow (hay que ponerlo) |

206 + `-c` concatena sobre el fichero existente. Si el servidor ignora
Range y manda 200, se pisa.

## `tls`

```text
www on
tls example.com
```

Ofrece 1.3 (`TLS_AES_128_GCM_SHA256`) y 1.2
(`ECDHE_RSA_WITH_AES_128_GCM_SHA256`). Imprime la versión negociada.
No trae la página. Sin CA.

En QEMU (2026-08-26) `tls example.com` → `handshake OK  TLS 1.3`.

HTTPS `curl -I https://…` puede devolver cuerpo vacío aunque el
handshake haya cerrado: las claves de aplicación van aparte. HTTP
claro (`http://`) está bien. Lo dejo aquí para no mentir.

## DNS, ping, nmap, traceroute

```text
dns google.com
ping 10.0.2.2
ping google.com          # ICMP; slirp a menudo lo come
nmap 10.0.2.2            # 21 22 23 25 53 80 110 143 443 445 3306 8080 8443
traceroute 1.1.1.1
```

`ping <alias>` sin punto es Rogex Wired L2 (ethertype `0x88B5`), no
ICMP. `chat` / `say` también. Eso es red de laboratorio entre dos
nodos RXos, no internet.

## Límites (red)

- Un socket.
- IPv4 only.
- Sin WiFi.
- Buffer cuerpo `WWW_BODY_MAX` = 192 KiB; RXFS escribe 64 KiB.
- TLS: AES-128-GCM, curva P-256. Ni ChaCha en este cliente, ni 0x1302.
- SNI sí. 0-RTT no. Session resume no.

— R.N.
