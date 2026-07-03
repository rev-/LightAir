#pragma once
struct Ticker {
    template <typename F, typename A> void attach_ms(uint32_t, F, A) {}
    template <typename F, typename A> void once_ms(uint32_t, F, A) {}
    void detach() {}
};
