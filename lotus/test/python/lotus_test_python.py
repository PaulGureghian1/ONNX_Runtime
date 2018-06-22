# -*- coding: UTF-8 -*-
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import numpy as np
import unittest
import lotus
import sys
from lotus.python._pybind_state import lotus_ostream_redirect

class TestInferenceSession(unittest.TestCase):

    def testRunModel(self):
        sess = lotus.InferenceSession("testdata/mul_1.pb")
        x = np.array([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]], dtype=np.float32)
        input_name = sess.get_inputs()[0].name
        self.assertEqual(input_name, "X")
        input_shape = sess.get_inputs()[0].shape
        self.assertEqual(input_shape, [3, 2])
        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "Y")
        output_shape = sess.get_outputs()[0].shape
        self.assertEqual(output_shape, [3, 2])
        res = sess.run([output_name], {input_name: x})
        output_expected = np.array([[1.0, 4.0], [9.0, 16.0], [25.0, 36.0]], dtype=np.float32)
        np.testing.assert_allclose(output_expected, res[0], rtol=1e-05, atol=1e-08)

    def testRunModel2(self):
        sess = lotus.InferenceSession("testdata/matmul_1.pb")
        x = np.array([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]], dtype=np.float32)
        input_name = sess.get_inputs()[0].name
        self.assertEqual(input_name, "X")
        input_shape = sess.get_inputs()[0].shape
        self.assertEqual(input_shape, [3, 2])
        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "Y")
        output_shape = sess.get_outputs()[0].shape
        self.assertEqual(output_shape, [3, 1])
        res = sess.run([output_name], {input_name: x})
        output_expected = np.array([[5.0], [11.0], [17.0]], dtype=np.float32)
        np.testing.assert_allclose(output_expected, res[0], rtol=1e-05, atol=1e-08)

    def testRunModelSymbolicInput(self):
        sess = lotus.InferenceSession("testdata/matmul_2.pb")
        x = np.array([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]], dtype=np.float32)
        input_name = sess.get_inputs()[0].name
        self.assertEqual(input_name, "X")
        input_shape = sess.get_inputs()[0].shape
        # Input X has an unknown dimension.
        self.assertEqual(input_shape, [None, 2])
        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "Y")
        output_shape = sess.get_outputs()[0].shape
        # Output X has an unknown dimension.
        self.assertEqual(output_shape, [None, 1])
        res = sess.run([output_name], {input_name: x})
        output_expected = np.array([[5.0], [11.0], [17.0]], dtype=np.float32)
        np.testing.assert_allclose(output_expected, res[0], rtol=1e-05, atol=1e-08)

    def testBooleanInputs(self):
        sess = lotus.InferenceSession("testdata/logicaland.pb")
        a = np.array([[True, True], [False, False]], dtype=np.bool)
        b = np.array([[True, False], [True, False]], dtype=np.bool)

        a_name = sess.get_inputs()[0].name
        self.assertEqual(a_name, "input:0")
        a_shape = sess.get_inputs()[0].shape
        self.assertEqual(a_shape, [2, 2])
        a_type = sess.get_inputs()[0].type
        self.assertEqual(a_type, 'tensor(bool)')

        b_name = sess.get_inputs()[1].name
        self.assertEqual(b_name, "input1:0")
        b_shape = sess.get_inputs()[1].shape
        self.assertEqual(b_shape, [2, 2])
        b_type = sess.get_inputs()[0].type
        self.assertEqual(b_type, 'tensor(bool)')

        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "output:0")
        output_shape = sess.get_outputs()[0].shape
        self.assertEqual(output_shape, [2, 2])
        output_type = sess.get_outputs()[0].type
        self.assertEqual(output_type, 'tensor(bool)')

        output_expected = np.array([[True, False], [False, False]], dtype=np.bool)
        res = sess.run([output_name], {a_name: a, b_name: b})
        np.testing.assert_equal(output_expected, res[0])

    def testStringInput1(self):
        sess = lotus.InferenceSession("testdata/identity_string.pb")
        x = np.array(['this', 'is', 'identity', 'test'], dtype=np.str).reshape((2,2))

        x_name = sess.get_inputs()[0].name
        self.assertEqual(x_name, "input:0")
        x_shape = sess.get_inputs()[0].shape
        self.assertEqual(x_shape, [2, 2])
        x_type = sess.get_inputs()[0].type
        self.assertEqual(x_type, 'tensor(string)')

        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "output:0")
        output_shape = sess.get_outputs()[0].shape
        self.assertEqual(output_shape, [2, 2])
        output_type = sess.get_outputs()[0].type
        self.assertEqual(output_type, 'tensor(string)')

        res = sess.run([output_name], {x_name: x})
        np.testing.assert_equal(x, res[0])

    def testStringInput2(self):
        sess = lotus.InferenceSession("testdata/identity_string.pb")
        x = np.array(['Olá', '你好', '여보세요', 'hello'], dtype=np.unicode).reshape((2,2))

        x_name = sess.get_inputs()[0].name
        self.assertEqual(x_name, "input:0")
        x_shape = sess.get_inputs()[0].shape
        self.assertEqual(x_shape, [2, 2])
        x_type = sess.get_inputs()[0].type
        self.assertEqual(x_type, 'tensor(string)')

        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "output:0")
        output_shape = sess.get_outputs()[0].shape
        self.assertEqual(output_shape, [2, 2])
        output_type = sess.get_outputs()[0].type
        self.assertEqual(output_type, 'tensor(string)')

        res = sess.run([output_name], {x_name: x})
        np.testing.assert_equal(x, res[0])

    def testConvAutoPad(self):
        sess = lotus.InferenceSession("testdata/conv_autopad.pb")
        x = np.array(25 * [1.0], dtype=np.float32).reshape((1,1,5,5))

        x_name = sess.get_inputs()[0].name
        self.assertEqual(x_name, "Input4")
        x_shape = sess.get_inputs()[0].shape
        self.assertEqual(x_shape, [1, 1, 5, 5])
        x_type = sess.get_inputs()[0].type
        self.assertEqual(x_type, 'tensor(float)')

        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "Convolution5_Output_0")
        output_shape = sess.get_outputs()[0].shape
        self.assertEqual(output_shape, [1, 1, 5, 5])
        output_type = sess.get_outputs()[0].type
        self.assertEqual(output_type, 'tensor(float)')

        res = sess.run([output_name], {x_name: x})
        output_expected = np.array([[[[24., 33., 33., 33., 20.],
                                      [27., 36., 36., 36., 21.],
                                      [27., 36., 36., 36., 21.],
                                      [27., 36., 36., 36., 21.],
                                      [12., 15., 15., 15.,  8.]]]], dtype=np.float32)
        np.testing.assert_allclose(output_expected, res[0])

    def testZipMapStringFloat(self):
        sess = lotus.InferenceSession("testdata/zipmap_stringfloat.pb")
        x = np.array([1.0, 0.0, 3.0, 44.0, 23.0, 11.0], dtype=np.float32).reshape((2,3))

        x_name = sess.get_inputs()[0].name
        self.assertEqual(x_name, "X")
        x_type = sess.get_inputs()[0].type
        self.assertEqual(x_type, 'tensor(float)')

        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "Z")
        output_type = sess.get_outputs()[0].type
        self.assertEqual(output_type, 'seq(map(string,tensor(float)))')

        output_expected = [{'class2': 0.0, 'class1': 1.0, 'class3': 3.0},
                           {'class2': 23.0, 'class1': 44.0, 'class3': 11.0}]
        res = sess.run([output_name], {x_name: x})
        self.assertEqual(output_expected, res[0])

    def testZipMapInt64Float(self):
        sess = lotus.InferenceSession("testdata/zipmap_int64float.pb")
        x = np.array([1.0, 0.0, 3.0, 44.0, 23.0, 11.0], dtype=np.float32).reshape((2,3))

        x_name = sess.get_inputs()[0].name
        self.assertEqual(x_name, "X")
        x_type = sess.get_inputs()[0].type
        self.assertEqual(x_type, 'tensor(float)')

        output_name = sess.get_outputs()[0].name
        self.assertEqual(output_name, "Z")
        output_type = sess.get_outputs()[0].type
        self.assertEqual(output_type, 'seq(map(int64,tensor(float)))')

        output_expected = [{10: 1.0, 20: 0.0, 30: 3.0}, {10: 44.0, 20: 23.0, 30: 11.0}]
        res = sess.run([output_name], {x_name: x})
        self.assertEqual(output_expected, res[0])

    def testRaiseWrongNumInputs(self):
        with self.assertRaises(ValueError) as context:
            sess = lotus.InferenceSession("testdata/logicaland.pb")
            a = np.array([[True, True], [False, False]], dtype=np.bool)
            res = sess.run([], {'input:0': a})

        self.assertTrue('Model requires 2 inputs' in str(context.exception))

    def testModelMeta(self):
        sess = lotus.InferenceSession('testdata/squeezenet/model.onnx')
        modelmeta = sess.get_modelmeta()
        self.assertEqual('onnx-caffe2', modelmeta.producer_name)
        self.assertEqual('squeezenet_old', modelmeta.graph_name)
        self.assertEqual('', modelmeta.domain)
        self.assertEqual('', modelmeta.description)

    def testConfigureSessionVerbosityLevel(self):
        so = lotus.SessionOptions()
        so.session_log_verbosity_level = 1

        # use lotus_ostream_redirect to redirect c++ stdout/stderr to python sys.stdout and sys.stderr
        with lotus_ostream_redirect(stdout=True, stderr=True):
          sess = lotus.InferenceSession("testdata/matmul_1.pb", sess_options=so)
          output = sys.stderr.getvalue()
          self.assertTrue('[I:Lotus:InferenceSession, inference_session' in output)

    def testConfigureRunVerbosityLevel(self):
        ro = lotus.RunOptions()
        ro.run_log_verbosity_level = 1
        ro.run_tag = "testtag123"

        # use lotus_ostream_redirect to redirect c++ stdout/stderr to python sys.stdout and sys.stderr
        with lotus_ostream_redirect(stdout=True, stderr=True):
            sess = lotus.InferenceSession("testdata/mul_1.pb")
            x = np.array([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]], dtype=np.float32)
            sess.run([], {'X': x}, run_options=ro)
            output = sys.stderr.getvalue()
            self.assertTrue('[I:Lotus:testtag123,' in output)

    def testProfilerWithSessionOptions(self):
        so = lotus.SessionOptions()
        so.enable_profiling = True
        sess = lotus.InferenceSession("testdata/mul_1.pb", sess_options=so)
        x = np.array([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]], dtype=np.float32)
        sess.run([], {'X': x})
        profile_file = sess.end_profiling()

        tags = ['pid', 'dur', 'ts', 'ph', 'X', 'name', 'args']
        with open(profile_file) as f:
            lines = f.readlines()
            self.assertTrue('[' in lines[0])
            for i in range(1, 8):
                for tag in tags:
                    self.assertTrue(tag in lines[i])
            self.assertTrue(']' in lines[8])

if __name__ == '__main__':
    unittest.main(module=__name__, buffer=True)
