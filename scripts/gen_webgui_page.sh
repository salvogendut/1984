#!/bin/sh
# Regenerate src/webgui_page.h from src/webgui_page.html.
# Run from the repository root after editing the HTML.
set -e
python3 - <<'EOF'
data = open("src/webgui_page.html", "rb").read()
with open("src/webgui_page.h", "w") as f:
    f.write("/* webgui_page.h — auto-generated from webgui_page.html. DO NOT EDIT.\n"
            " * Regenerate with: ./scripts/gen_webgui_page.sh\n */\n")
    f.write("unsigned char webgui_page[] = {\n")
    for i in range(0, len(data), 12):
        f.write("  " + ", ".join("0x%02x" % b for b in data[i:i+12]) + ",\n")
    f.write("};\n")
    f.write("unsigned int webgui_page_len = %d;\n" % len(data))
print("wrote src/webgui_page.h (%d bytes)" % len(data))
EOF
