// Copyright 2022-2024 The Parca Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package bpfprograms

import (
	"embed"
	"fmt"
	"io"
	"runtime"
)

type ProfilerModuleType int

const (
	NativeModule ProfilerModuleType = iota
	RbperfModule
	PyperfModule
	JVMModule
	LuaModule
)

var (
	//go:embed objects/*
	objects embed.FS

	// native programs.
	NativeProgramFD           = uint64(0)
	RbperfEntrypointProgramFD = uint64(1)
	PyperfEntrypointProgramFD = uint64(2)
	JVMEntrypointProgramFD    = uint64(3)
	LuaEntrypointProgramFD    = uint64(4)

	// rbperf programs.
	RubyUnwinderProgramFD = uint64(0)
	// pyperf programs.
	PythonUnwinderProgramFD = uint64(0)
	// jvm programs.
	JavaUnwinderProgramFD = uint64(0)
	// lua programs.
	LuaUnwinderProgramFD = uint64(0)

	ProgramName               = "entrypoint"
	NativeUnwinderProgramName = "native_unwind"
)

var ProgNames = []string{"native.bpf.o", "rbperf.bpf.o", "pyperf.bpf.o", "jvm.bpf.o", "lua.bpf.o"}

func OpenNative() ([]byte, error) {
	return open(fmt.Sprintf("objects/%s/%s", runtime.GOARCH, ProgNames[NativeModule]))
}

func OpenRbperf() ([]byte, error) {
	return open(fmt.Sprintf("objects/%s/%s", runtime.GOARCH, ProgNames[RbperfModule]))
}

func OpenPyperf() ([]byte, error) {
	return open(fmt.Sprintf("objects/%s/%s", runtime.GOARCH, ProgNames[PyperfModule]))
}

func OpenJVM() ([]byte, error) {
	return open(fmt.Sprintf("objects/%s/%s", runtime.GOARCH, ProgNames[JVMModule]))
}

func OpenLua() ([]byte, error) {
	return open(fmt.Sprintf("objects/%s/%s", runtime.GOARCH, ProgNames[LuaModule]))
}

func open(file string) ([]byte, error) {
	f, err := objects.Open(file)
	if err != nil {
		return nil, fmt.Errorf("failed to open BPF object: %w", err)
	}

	// Note: no need to close this file, it's a virtual file from embed.FS, for
	// which Close is a no-op.

	bpfObj, err := io.ReadAll(f)
	if err != nil {
		return nil, fmt.Errorf("failed to read BPF object: %w", err)
	}

	return bpfObj, nil
}
