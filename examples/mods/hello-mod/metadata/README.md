# Hello Mod

The worked example of a mod that ships a **script** rather than only assets.

`metadata/info.json` names it:

```json
{ "name": "Hello Mod", "script": "hello.mjs" }
```

That path is relative to `metadata/`, which is the one directory in a mod that is not game
content - so a script cannot collide with an asset, and every mod's scripts can be called
whatever their author likes. `hello.mjs` imports `./lib/greeting.mjs` the ordinary way; both
are read when the mod loads, so the import works inside a `.zip` too, where neither file has
a path on disk.

Nothing here is enabled by being present. A profile's `boot.mjs` has to name this directory:

```js
mods.enable(mods.load("mods/hello-mod"));
```

The script then runs at that moment - which from a boot module is inside `WinMain`, before the
game has a console, resource strings or menus. That is why the menu item is registered from
`setup_menus` and not at module scope: the host calls it once the front end exists.
