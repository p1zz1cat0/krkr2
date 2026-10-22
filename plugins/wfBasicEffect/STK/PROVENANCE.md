STK v4.6.0, thestk/stk@11d1dc54f31e05ee71c8e63d116041dd2a8e7442.
Source: https://github.com/thestk/stk/tree/11d1dc54f31e05ee71c8e63d116041dd2a8e7442

Only FreeVerb and its dependencies are included. LICENSE applies.
Local changes: FreeVerb takes an explicit instance sample rate and scales local delay lengths, so constructing one instance cannot change later instances. clear() also clears comb low-pass histories for deterministic reset. The processing and mix equations remain upstream.
