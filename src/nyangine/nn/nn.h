#pragma once

/*
 * Tensors and autograd first: layers are built on them, and optimizers on both.
 *
 * nn_neat.h is independent of all three — NEAT evolves topology and weights directly and has no use
 * for a gradient. The two live side by side rather than one on top of the other.
 */
#include "nyangine/nn/nn_dqn.h"
#include "nyangine/nn/nn_layer.h"
#include "nyangine/nn/nn_optim.h"
#include "nyangine/nn/nn_tensor.h"

#include "nyangine/nn/nn_neat.h"
