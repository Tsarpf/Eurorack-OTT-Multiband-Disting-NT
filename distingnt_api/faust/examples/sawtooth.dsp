declare guid "ExSw";
declare name "Sawtooth";
declare description "Sawtooth VCO";

import("stdfaust.lib");

freq = nentry( "Frequency", 440, 20, 20000, 0.01 ) : si.smoo;
gain = nentry( "Gain", 1, 0, 1, 0.01 ) : si.smoo;

process = os.sawtooth( freq ) * gain;
