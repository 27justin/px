# Tuples

# Sequential

```px
let t: (i32, i32) = (1, 42);
print("%i,%i", t.0, t.1);
```

```text
1,42
```

# Named

```px
let t: (r: u8, g: u8, b: u8) = (r: 255, g: 0, b: 127);
print("RGB(0x%02X, 0x%02X, 0x%02X)", t.r, t.g, t.b);
```

```text
RGB(0xFF, 0x00, 0x7F)
```

