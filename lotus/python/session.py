## @package session
# Module lotus.python.session
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import sys
import logging
import ctypes
import os

logger = logging.getLogger(__name__)
from lotus.python import _pybind_state as C

class InferenceSession:
  def __init__(self, path, sess_options=None):
    if sess_options:
      self._sess = C.InferenceSession(
        sess_options, C.get_session_initializer())
    else:
      self._sess = C.InferenceSession(
        C.get_session_initializer(), C.get_session_initializer())
    self._sess.load_model(path)
    self._inputs_meta = self._sess.inputs_meta
    self._outputs_meta = self._sess.outputs_meta
    self._model_meta = self._sess.model_meta

  def get_inputs(self) :
    return self._inputs_meta

  def get_outputs(self) :
    return self._outputs_meta

  def get_modelmeta(self) :
    return self._model_meta

  def run(self, output_names, input_feed, run_options=None) :
    num_required_inputs = len(self._inputs_meta)
    num_inputs = len(input_feed)
    if num_inputs != num_required_inputs:
        raise ValueError("Model requires {} inputs. Input Feed contains {}".format(num_required_inputs,
                                                                                   num_inputs))
    if not output_names:
      output_names = [ output.name for output in self._outputs_meta ]
    return self._sess.run(output_names, input_feed, run_options)

  def end_profiling(self) :
    return self._sess.end_profiling()

if __name__ == "__main__":
  if not 'InferenceSession' in dir(C):
    raise RuntimeError('Failed to bind the native module -lotus_pybind11_state.pyd.')

