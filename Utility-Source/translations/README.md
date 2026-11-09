# Installer translations

The installer's text is translated with Qt Linguist. Each language is one file,
`glassboot_<language>.ts` (for example `glassboot_de.ts`, `glassboot_pt_BR.ts`). Every
`.ts` file here is compiled into the program, which then picks the one that matches the
system language. `glassboot_en.ts` is the template: it lists every string, untranslated.

## Add a language

```sh
cd Utility-Source
cp translations/glassboot_en.ts translations/glassboot_de.ts
# set language="de" in the <TS> line, then translate:
linguist translations/glassboot_de.ts      # or any .ts editor, or a text editor
```

Rebuild the installer and start it with that language (`LANGUAGE=de` on Linux, or the
Windows display language) to check the result.

## After changing strings in the source

```sh
cmake --build <build-dir> --target update_translations
```

This refreshes every `.ts` file. New strings appear as unfinished, and strings that were
removed are marked obsolete.

Building translations needs Qt's Linguist tools (`qt6-l10n-tools` and `qt6-tools-dev` on
Debian/Ubuntu). Without them the installer builds in English only.
