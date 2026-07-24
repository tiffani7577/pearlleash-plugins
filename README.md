# pearlleash-plugins

Permanent browsable archive of every PearlLeash / PlugForge generated JUCE project.

Each successful forge job commits the full exporter output under a folder named after the plugin:

```
pearlleash-plugins/
├── GutterVerb/
│   ├── Source/
│   ├── CMakeLists.txt
│   ├── BUILD_STEPS.md
│   └── PROVENANCE.md
└── …
```

Specs also live in `tiffani7577/womanus_vs_5-/plugforge/specs/{PluginName}/` (versioned + latest.plugforge.json). This repo holds the **full source** so it survives Railway redeploys.
