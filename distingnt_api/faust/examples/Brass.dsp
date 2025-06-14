declare guid "ExBr";
declare name "Brass";
declare description "Simple brass instrument physical model with physical parameters.";
declare license "MIT";
declare copyright "(c)Romain Michon, CCRMA (Stanford University), GRAME";
declare URL "https://faustdoc.grame.fr/examples/physicalModeling/";

import("stdfaust.lib");

process = pm.brass_ui <: _,_;
