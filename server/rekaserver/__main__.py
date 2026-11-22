import os
import socket

import uvicorn


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8000))
    print(f"\n  Dashboard:         http://127.0.0.1:{port}")
    print(f"  For a real board:  http://{lan_ip()}:{port}  (type this as the server in the Wi-Fi setup page)")
    from . import firmware
    if firmware.DIR:
        found = ", ".join(f"{r['version']} {r.get('channel')}" for r in firmware.releases()) or "none valid yet"
        print(f"  Firmware updates:  {firmware.DIR} ({found})")
    print()
    uvicorn.run("rekaserver.app:app", host="0.0.0.0", port=port, log_level="warning")
