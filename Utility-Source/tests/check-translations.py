"""Check shipped Qt catalogs for untranslated strings and broken placeholders."""
from collections import Counter
from pathlib import Path
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
for path in root.glob("translations/glassboot_*.ts"):
    if path.stem == "glassboot_en":
        continue  # English is the source/template catalog.
    count = 0
    for message in ET.parse(path).findall(".//message"):
        translation = message.find("translation")
        if translation is not None and translation.get("type") in {"obsolete", "vanished"}:
            continue
        assert translation is not None and translation.get("type") != "unfinished", (path, ET.tostring(message))
        source = message.findtext("source", "")
        forms = translation.findall("numerusform") if message.get("numerus") == "yes" else [translation]
        assert forms, (path, source)
        placeholders = Counter(re.findall(r"%[1-9n]", source))
        for form in forms:
            assert form.text and Counter(re.findall(r"%[1-9n]", form.text)) == placeholders, (path, source, form.text)
        count += 1
    print(f"{path.name}: {count} translated messages checked")
