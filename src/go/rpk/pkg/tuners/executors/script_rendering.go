// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package executors

import (
	"bufio"
	"fmt"
	"os"

	"github.com/redpanda-data/redpanda/src/go/rpk/pkg/tuners/executors/commands"
	"github.com/spf13/afero"
)

type scriptRenderingExecutor struct {
	deferred error
	writer   *bufio.Writer
}

// FIXME: @david
// This should also return an error.
func NewScriptRenderingExecutor(fs afero.Fs, filename string) Executor {
	file, err := fs.OpenFile(filename, os.O_CREATE|os.O_TRUNC|os.O_RDWR, 0o755)
	if err != nil {
		return &scriptRenderingExecutor{
			deferred: err,
			writer:   nil,
		}
	}
	header := `#!/bin/bash

# Redpanda Tuning Script
# ----------------------------------
# This file was autogenerated by RPK

`
	w := bufio.NewWriter(file)
	_, _ = fmt.Fprint(w, header)
	_ = w.Flush()
	return &scriptRenderingExecutor{
		deferred: nil,
		writer:   w,
	}
}

func (e *scriptRenderingExecutor) Execute(cmd commands.Command) error {
	err := cmd.RenderScript(e.writer)
	if err != nil {
		return err
	}
	return e.writer.Flush()
}

func (*scriptRenderingExecutor) IsLazy() bool {
	return true
}
