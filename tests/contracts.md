# Contracts

Contracts are a way to do type-erasure at the language level, they
allows you to decay concrete types into a type that has just a set
number of traits it full-fills.

Making it possible to store data based on functionality, independent
of actual size of the data structure.


# sizeof should yield 2 * any

```px
c := contract {
    to_string := fn (!self) -> !u8
}

// ---

print("%llu", sizeof(c));
```

```text
16
```

