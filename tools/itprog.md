# IT DMA Program

- Channel: 0
- Sample Rate: 48000 Hz
- Blocking: Yes
- Schedule: `[8, 8, 8, 8, 8, 8, 0, 0]`

```mermaid
%%{init: {"theme": "base", "themeVariables": {"background": "#f5f7fa", "primaryColor": "#a8f4a2", "primaryTextColor": "#0f2d0f", "lineColor": "#4a5568", "secondaryColor": "#ffd1dc", "tertiaryColor": "#d9e2ec", "fontFamily": "SFMono-Regular,Menlo,Consolas,monospace", "edgeLabelBackground": "#f5f7fa"}, "flowchart": {"htmlLabels": true, "curve": "monotoneX"}}}%%
graph LR
    B0["DATA<br/>Z=3<br/>0x80000000"]
    style B0 fill:#90EE90
    B1["DATA<br/>Z=3<br/>0x80000030"]
    style B1 fill:#90EE90
    B2["DATA<br/>Z=3<br/>0x80000060"]
    style B2 fill:#90EE90
    B3["DATA<br/>Z=3<br/>0x80000090"]
    style B3 fill:#90EE90
    B4["DATA<br/>Z=3<br/>0x800000C0"]
    style B4 fill:#90EE90
    B5["DATA<br/>Z=3<br/>0x800000F0"]
    style B5 fill:#90EE90
    B6["NO-DATA<br/>Z=2<br/>0x80000120"]
    style B6 fill:#FFB6C1
    B7["NO-DATA<br/>Z=2<br/>0x80000140"]
    style B7 fill:#FFB6C1
    B0 --> B1
    B1 --> B2
    B2 --> B3
    B3 --> B4
    B4 --> B5
    B5 --> B6
    B6 --> B7
    B7 --> B0
```

> **Note:** Some renderers (e.g., GitHub preview) ignore the inline Mermaid init block. If the theme does not appear, view in a renderer that supports `%%{init: ...}%%`.
