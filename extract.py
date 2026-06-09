import base64
with open("encrypt-key.pem", "r") as f:
    lines = f.read().splitlines()
b64 = "".join(lines[1:-1])
der = base64.b64decode(b64)
c_array = ", ".join(f"0x{b:02x}" for b in der)
with open("encrypt-priv.c", "w") as f:
    f.write(f"const unsigned char ecdsa_priv_key[] = {{ {c_array} }};\n")
    f.write(f"const unsigned int ecdsa_priv_key_len = {len(der)};\n")
