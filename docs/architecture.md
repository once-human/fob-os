# Architecture

Five layers, each only talking to the one beneath it:

1. **modes**: carry, cards, music, inbox, remote, claude, play, tools, settings
2. **runtime**: dial and navigation, per-mode memory, gestures, animation engine (springs), haptic patterns
3. **renderer**: vector display list (rounded rectangles, rings, arcs, capsules, anti-aliased text), dirty rectangles, 12-line strips, double-buffered DMA
4. **drivers and services**: quad-SPI + DMA, touch (I2C), crown (1 kHz), motor, Bluetooth (phone services, laptop channel, wireless updates)
5. **hardware**

The renderer and the UI are portable C (`*.inc`) that compile both on the watch and in the laptop simulator in `sim/`; every screenshot there checks that the incremental redraw equals a full redraw.
Full diagrams and reasoning: onkaryaglewad.in/fob/book.html.
