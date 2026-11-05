// floating point in its own file so the compiler can't fold it
float fsq(float x) {
	float r = x;
	for (int i = 0; i < 8; i++) r = 0.5f * (r + x / r);
	return r;
}
