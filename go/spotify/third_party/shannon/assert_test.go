package shannon

import (
	"reflect"
	"testing"
)

type testAssert struct{ t *testing.T }

func newTestAssert(t *testing.T) testAssert { return testAssert{t} }
func (a testAssert) Equal(want, got interface{}) {
	a.t.Helper()
	if !reflect.DeepEqual(want, got) {
		a.t.Fatalf("want %v got %v", want, got)
	}
}
func (a testAssert) Nil(v interface{}) {
	a.t.Helper()
	if v != nil {
		a.t.Fatalf("expected nil, got %v", v)
	}
}
func (a testAssert) NotNil(v interface{}) {
	a.t.Helper()
	if v == nil {
		a.t.Fatal("unexpected nil")
	}
}
