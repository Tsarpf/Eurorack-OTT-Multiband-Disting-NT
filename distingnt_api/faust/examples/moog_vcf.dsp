declare guid "ExMO";
declare name "Moog VCF";
declare description "Moog filter, CV and audio inputs";

import("stdfaust.lib");

fr = hslider( "[0]Frequency", 500, 10, 7000, 1 ) : si.smoo;
res = hslider( "[1]Q", 0.5, 0.0, 1.0, 0.01 ) : si.smoo;

process = fr * pow(2,_),_ : ve.moog_vcf_2b( res );
