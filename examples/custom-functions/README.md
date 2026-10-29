# More Functions Custom Pages

Put custom `*.html` files in `/data/plugins/zwrt-devui/ui/functions/`.
DevUI scans that directory and adds one entry per file to the "更多功能" page.

This example is kept outside the packaged `ui/` directory on purpose. Copy it
into `ui/functions/` only when you want it to appear on the device.

If the file has a `<title>...</title>`, that title is used as the entry name.
Otherwise the filename without `.html` is used. Custom pages are opened as
second-level pages with the standard status bar and back button automatically
wrapped around their body content.

Touch actions are triggered by anchors only:

```html
<a href="act:theme" class="card lockcard">Tap this card</a>
<a href="act:sub:wifi.html" class="card resetbtn2">Open WiFi page</a>
<div class="seg seg4">
  <a href="act:backfunc" class="segc">Back</a>
  <a href="act:sub:wifi.html" class="segc">WiFi</a>
  <a href="act:sub:sms.html" class="segc">SMS</a>
  <a href="act:func:example.html" class="segc seg-on">Custom</a>
</div>
```

The clickable area is the rendered rectangle of the `<a>` element. Do not rely
on JavaScript, `onclick`, `<button>`, or forms; the DevUI touch handler only
dispatches `href="act:..."` links.
