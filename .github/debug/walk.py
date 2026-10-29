import json, re, urllib.request
API = "https://components.espressif.com/api/components/"
seen, keys = set(), set()
def walk(full, depth=0):
    if full in seen or depth > 6:
        return
    seen.add(full)
    with urllib.request.urlopen(API + full, timeout=30) as r:
        d = json.load(r)
    vs = d.get("versions", [])
    for v in vs[:3]:
        for dep in v.get("dependencies", []):
            for rule in (dep.get("rules") or []) + (dep.get("matches") or []):
                cond = rule.get("if", "")
                if "$CONFIG" in cond:
                    keys.update(re.findall(r"\$CONFIG\{(\w+)\}", cond))
                    print(full, v["version"], "->", dep.get("namespace"), dep.get("name"), "if", cond)
    if vs:
        for dep in vs[0].get("dependencies", []):
            if dep.get("namespace") and dep.get("source", "service") == "service":
                walk(f"{dep['namespace']}/{dep['name']}", depth + 1)
for c in ["waveshare/esp32_p4_wifi6_touch_lcd_xc", "espressif/esp_wifi_remote", "lvgl/lvgl"]:
    walk(c)
print("KEYS", sorted(keys))
