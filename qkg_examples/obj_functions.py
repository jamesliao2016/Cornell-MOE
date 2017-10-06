import numpy
import math

class BraninNoNoise(object):
    def __init__(self):
        self._dim = 2
        self._search_domain = numpy.array([[0, 15], [-5, 15]])
        self._num_init_pts = 3
        self._sample_var = 0.0
        self._min_value = 0.397887
        self._num_fidelity = 0

    def evaluate_true(self, x):
        """ This function is usually evaluated on the square x_1 \in [-5, 10], x_2 \in [0, 15]. Global minimum
        is at x = [-pi, 12.275], [pi, 2.275] and [9.42478, 2.475] with minima f(x*) = 0.397887.

            :param x[2]: 2-dim numpy array
        """
        a = 1
        b = 5.1 / (4 * pow(numpy.pi, 2.0))
        c = 5 / numpy.pi
        r = 6
        s = 10
        t = 1 / (8 * numpy.pi)
        return numpy.array([(a * pow(x[1] - b * pow(x[0], 2.0) + c * x[0] - r, 2.0) + s * (1 - t) * numpy.cos(x[0]) + s),
                (2*a*(x[1] - b * pow(x[0], 2.0) + c * x[0] - r) * (-2* b * x[0] + c) + s * (1 - t) * (-numpy.sin(x[0]))),
                (2*a*(x[1] - b * pow(x[0], 2.0) + c * x[0] - r))])

    def evaluate(self, x):
        return self.evaluate_true(x)

class RosenbrockNoNoise(object):
    def __init__(self):
        self._dim = 3
        self._search_domain = numpy.repeat([[-2., 2.]], 3, axis=0)
        self._num_init_pts = 3
        self._sample_var = 0.25
        self._min_value = 0.0
        self._num_fidelity = 0

    def evaluate_true(self, x):
        """ Global minimum is 0 at (1, 1, 1)

            :param x[3]: 3-dimension numpy array
        """
        value = 0.0
        for i in range(self._dim-1):
            value += pow(1. - x[i], 2.0) + 100. * pow(x[i+1] - pow(x[i], 2.0), 2.0)
        results = [value]
        for i in range(self._dim-1):
            results += [2.*(x[i]-1) - 400.*x[i]*(x[i+1]-pow(x[i], 2.0))]
        results += [200. * (x[self._dim-1]-pow(x[self._dim-2], 2.0))]
        return numpy.array(results)

    def evaluate(self, x):
        return self.evaluate_true(x)

class Hartmann3(object):
    def __init__(self):
        self._dim = 3
        self._search_domain = numpy.repeat([[0., 1.]], self._dim, axis=0)
        self._num_init_pts = 3
        self._sample_var = 0.25
        self._min_value = -3.86278
        self._num_fidelity = 0

    def evaluate_true(self, x):
        """ domain is x_i \in (0, 1) for i = 1, ..., 3
            Global minimum is -3.86278 at (0.114614, 0.555649, 0.852547)

            :param x[3]: 3-dimension numpy array with domain stated above
        """
        alpha = numpy.array([1.0, 1.2, 3.0, 3.2])
        A = numpy.array([[3., 10., 30.], [0.1, 10., 35.], [3., 10., 30.], [0.1, 10., 35.]])
        P = 1e-4 * numpy.array([[3689, 1170, 2673], [4699, 4387, 7470], [1091, 8732, 5547], [381, 5743, 8828]])
        value = 0.0
        for i in range(4):
            inner_value = 0.0
            for j in range(self._dim):
                inner_value -= A[i, j] * pow(x[j] - P[i, j], 2.0)
            value -= alpha[i] * numpy.exp(inner_value)
        return numpy.array([value])

    def evaluate(self, x):
        t = self.evaluate_true(x)
        return t