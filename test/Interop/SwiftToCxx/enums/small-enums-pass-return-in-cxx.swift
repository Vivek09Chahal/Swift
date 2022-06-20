// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Enums -clang-header-expose-public-decls -emit-clang-header-path %t/enums.h
// RUN: %FileCheck %s < %t/enums.h

// RUN: %check-interop-cxx-header-in-clang(%t/enums.h -Wno-unused-private-field -Wno-unused-function)

public enum Tiny {
    case first
    case second
}

public func makeTiny(_ x: Int) -> Tiny {
    return x >= 0 ? .first : .second
}

public func printTiny(_ en: Tiny) {
    switch en {
    case .first:
        print("Tiny.first")
    case .second:
        print("Tiny.second")
    }
}

public func passThroughTiny(_ en: Tiny) -> Tiny {
    return en
}

public func inoutTiny(_ en: inout Tiny, _ x: Int) {
    if x >= 0 {
        en = .first
    } else {
        en = .second
    }
}

public enum Small {
    case first(Int)
    case second(Double)
}

public func makeSmall(_ x: Int) -> Small {
    return x >= 0 ? .first(x) : .second(Double(-x))
}

public func printSmall(_ en: Small) {
    switch en {
    case let .first(x):
        print("Small.first(\(x))")
    case let .second(x):
        print("Small.second(\(x))")
    }
}

public func passThroughSmall(_ en: Small) -> Small {
    return en
}

public func inoutSmall(_ en: inout Small, _ x: Int) {
    if x >= 0 {
        en = .first(x)
    } else {
        en = .second(Double(x - 1))
    }
}

// CHECK: SWIFT_EXTERN void $s5Enums10inoutSmallyyAA0C0Oz_SitF(char * _Nonnull en, ptrdiff_t x) SWIFT_NOEXCEPT SWIFT_CALL; // inoutSmall(_:_:)
// CHECK: SWIFT_EXTERN void $s5Enums9inoutTinyyyAA0C0Oz_SitF(char * _Nonnull en, ptrdiff_t x) SWIFT_NOEXCEPT SWIFT_CALL; // inoutTiny(_:_:)
// CHECK: SWIFT_EXTERN struct swift_interop_stub_Enums_Small $s5Enums9makeSmallyAA0C0OSiF(ptrdiff_t x) SWIFT_NOEXCEPT SWIFT_CALL; // makeSmall(_:)
// CHECK: SWIFT_EXTERN struct swift_interop_stub_Enums_Tiny $s5Enums8makeTinyyAA0C0OSiF(ptrdiff_t x) SWIFT_NOEXCEPT SWIFT_CALL; // makeTiny(_:)
// CHECK: SWIFT_EXTERN struct swift_interop_stub_Enums_Small $s5Enums16passThroughSmallyAA0D0OADF(struct swift_interop_stub_Enums_Small en) SWIFT_NOEXCEPT SWIFT_CALL; // passThroughSmall(_:)
// CHECK: SWIFT_EXTERN struct swift_interop_stub_Enums_Tiny $s5Enums15passThroughTinyyAA0D0OADF(struct swift_interop_stub_Enums_Tiny en) SWIFT_NOEXCEPT SWIFT_CALL; // passThroughTiny(_:)
// CHECK: SWIFT_EXTERN void $s5Enums10printSmallyyAA0C0OF(struct swift_interop_stub_Enums_Small en) SWIFT_NOEXCEPT SWIFT_CALL; // printSmall(_:)
// CHECK: SWIFT_EXTERN void $s5Enums9printTinyyyAA0C0OF(struct swift_interop_stub_Enums_Tiny en) SWIFT_NOEXCEPT SWIFT_CALL; // printTiny(_:)
// CHECK: class Small final {
// CHECK: class Tiny final {

// CHECK:      inline void inoutSmall(Small& en, swift::Int x) noexcept {
// CHECK-NEXT:   return _impl::$s5Enums10inoutSmallyyAA0C0Oz_SitF(_impl::_impl_Small::getOpaquePointer(en), x);
// CHECK-NEXT: }

// CHECK:      inline void inoutTiny(Tiny& en, swift::Int x) noexcept {
// CHECK-NEXT:   return _impl::$s5Enums9inoutTinyyyAA0C0Oz_SitF(_impl::_impl_Tiny::getOpaquePointer(en), x);
// CHECK-NEXT: }

// CHECK:      inline Small makeSmall(swift::Int x) noexcept SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_Small::returnNewValue([&](char * _Nonnull result) {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Enums_Small(result, _impl::$s5Enums9makeSmallyAA0C0OSiF(x));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK:      inline Tiny makeTiny(swift::Int x) noexcept SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_Tiny::returnNewValue([&](char * _Nonnull result) {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Enums_Tiny(result, _impl::$s5Enums8makeTinyyAA0C0OSiF(x));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK:      inline Small passThroughSmall(const Small& en) noexcept SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_Small::returnNewValue([&](char * _Nonnull result) {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Enums_Small(result, _impl::$s5Enums16passThroughSmallyAA0D0OADF(_impl::swift_interop_passDirect_Enums_Small(_impl::_impl_Small::getOpaquePointer(en))));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK:      inline Tiny passThroughTiny(const Tiny& en) noexcept SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_Tiny::returnNewValue([&](char * _Nonnull result) {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Enums_Tiny(result, _impl::$s5Enums15passThroughTinyyAA0D0OADF(_impl::swift_interop_passDirect_Enums_Tiny(_impl::_impl_Tiny::getOpaquePointer(en))));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK:      inline void printSmall(const Small& en) noexcept {
// CHECK-NEXT:   return _impl::$s5Enums10printSmallyyAA0C0OF(_impl::swift_interop_passDirect_Enums_Small(_impl::_impl_Small::getOpaquePointer(en)));
// CHECK-NEXT: }

// CHECK:      inline void printTiny(const Tiny& en) noexcept {
// CHECK-NEXT:   return _impl::$s5Enums9printTinyyyAA0C0OF(_impl::swift_interop_passDirect_Enums_Tiny(_impl::_impl_Tiny::getOpaquePointer(en)));
// CHECK-NEXT: }
