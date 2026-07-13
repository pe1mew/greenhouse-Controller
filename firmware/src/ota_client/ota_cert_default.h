/**
 * @file ota_cert_default.h
 * @brief ROTA — firmware-embedded default pinned server certificate (R-A04).
 *
 * The public self-signed certificate for the OTA server (ota.rfsee.net,
 * generated 2026-07-13, valid ~20 y). Used as the pinned cert when no
 * operator certificate has been uploaded via the GUI (rota_tds.md R-A03/A04).
 * PUBLIC certificate only — the private key never leaves the operator secret
 * store. Regenerate this header from the credentials repo's ota_server.pem if
 * the server cert is ever rotated.
 *
 * @since 2.2.0 (ROTA)
 */
#pragma once

static const char OTA_DEFAULT_CERT_PEM[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIESTCCArGgAwIBAgIUX2vfv38nIEafMYDrB3PeBiSF278wDQYJKoZIhvcNAQEL
BQAwGDEWMBQGA1UEAwwNb3RhLnJmc2VlLm5ldDAeFw0yNjA3MTMwNzI0MDBaFw00
NjA3MDgwNzI0MDBaMBgxFjAUBgNVBAMMDW90YS5yZnNlZS5uZXQwggGiMA0GCSqG
SIb3DQEBAQUAA4IBjwAwggGKAoIBgQCRVubf3xfuU0rC2KvBe2CsJGhcSkRzLiHE
IIQAi9ExUwdVovAXPvaDl2C3M+b67G+19oGMct1wu4LZc6cfqFqKCmWTzQc2b1K0
DyGludMhIqU1PLQak+XM/5roIRTXI+FAFzh4/J/CpoqFmBlfCF/ZIFXLynbNoLoR
0Kc07Ef985s+pKIUd38VHnpSvOR2TZeUsgLuLQLV9L3l6aBPU7tESbT2gwBQux55
8kAEEYCVyJVJncCmynx+3DIWHNlU+Cy4YUNxGx6JqrTktRB7Yz/TijShuAp1UCaV
ZtG+nGbmq7NDmvXWC9SMLtmxin3q6CYedENRE/HxzxFL6N+TIMkz3/HuuOHQcjvi
dQ4EhN7e6dcb7Fa21052CqeF+r0ITDYDNYcJ1UszSXjf3qDco3BaHQZfU0/3jepy
War63NUIvr5zlDgRpc7mEDRk5kgW1dHgk5tAOanp1QalWdkIASV7NDc/9gpTKiWy
yv9t8/53FLh4y+WFsBJRQPaIEsFZeasCAwEAAaOBijCBhzAdBgNVHQ4EFgQU1Mao
vojbk58+8jlm9eOXnRvkLrkwHwYDVR0jBBgwFoAU1Maovojbk58+8jlm9eOXnRvk
LrkwGAYDVR0RBBEwD4INb3RhLnJmc2VlLm5ldDAJBgNVHRMEAjAAMAsGA1UdDwQE
AwIFoDATBgNVHSUEDDAKBggrBgEFBQcDATANBgkqhkiG9w0BAQsFAAOCAYEAXN5/
oJMucq3EA4VJJlHFbrorbVTwbfyAwbRUSOrM8vNCsI/+SGFPT/4t5xqVfKQ1jwV0
AqY+5E4p5x9EcT/HaEUWfbC/dvahWfxRag2UV/1lmSXQLBxl/esp6+ewn4P1Am4R
R0vnp22q1ovdO+1gwm0EP0LVXVxmVXqdJ0+QLXveI2P7cdy+iR0NqXnwgfILkAWO
hB0sICdApz4o0qUJqF/n3fJmVhMe/C/yhXUf1COV8jl9oLzX0CpZ2biYMsC0OdaP
f3gDWabp0ybhHZOmkQ9GOG4HfRiRKJT99Arw/LOnpXtRHy4lp/+b+CTXmdb8dht5
xtZpDlCuKCpVp3+zEJ4gjoUVUM1S00Au2QgsSxUjiAwRYtf1rpmAzPwZnDtVOiNn
bpd/0r1q5C8qvVsjoU30655WaCJEUuoMdBg8UcoTmgWNU25nS9FHcmoQ11j92D6y
3DllVnphPpWaxJw35U54voNv0lNCGRRFMhOYBUrAoyN29hqSLbEtg0M/iH2/
-----END CERTIFICATE-----
)PEM";
