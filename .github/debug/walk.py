import json, urllib.request
API = "https://components.espressif.com/api/components/"
seen = set()
def get(full):
    with urllib.request.urlopen(API + full, timeout=30) as r:
        return json.load(r)
def walk(full, depth=0):
    if full in seen or depth > 5:
        return
    seen.add(full)
    try:
        d = get(full)
    except Exception as e:
        print("  " * depth, full, "ERR", e); return
    vs = d.get("versions", [])
    print("  " * depth, full, "latest", [(v["version"], v.get("created_at", "")[:10]) for v in vs[:5]])
    for v in vs[:12]:
        for dep in v.get("dependencies", []):
            if "FREETYPE" in json.dumps(dep):
                print("  " * depth, "  FREETYPE in", full, v["version"], v.get("created_at", "")[:10], json.dumps(dep)[:400])
    if vs:
        for dep in vs[0].get("dependencies", []):
            ns, nm = dep.get("namespace"), dep.get("name")
            if ns and nm and dep.get("source", "service") == "service":
                walk(f"{ns}/{nm}", depth + 1)
for c in ["waveshare/esp32_p4_wifi6_touch_lcd_xc", "espressif/esp_wifi_remote"]:
    walk(c)
