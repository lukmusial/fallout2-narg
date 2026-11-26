### Known issues

- Talking heads, movies, the mouse cursors (including the dragged inventory item), the action menu, indicator boxes and endgame slides are the original art, doubled.
- The first visit to a big town takes up to about 0.8 s behind the loading screen while its art is upscaled (0.1-0.3 s once it is in `data/hd/cache`). Maps a town links to are not prepared ahead.
- A weapon drawn for the first time in combat is upscaled when first shown (about 1 ms per frame).
- Walls are upscaled without their neighbours; ground variants need an HD art pack.
- HD needs `[screen] scale=2` and a screen of at least 1280x960; with `scale=1` the game runs in the original look.
