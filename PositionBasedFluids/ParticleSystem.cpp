#include "ParticleSystem.h"

using namespace std;

static const float deltaT = 0.05f;
static const float PI = 3.14159265358979323846f;
static const glm::vec3 GRAVITY = glm::vec3(0, -9.8f, 0);
static const int PRESSURE_ITERATIONS = 4;
static const float H = 1.1f;
static const float KPOLY = 315 / (64 * PI * glm::pow(H, 9));
static const float SPIKY = 45 / (PI * glm::pow(H, 6));
static const float VISC = 15 / (2 * PI * (H * H * H));
static const float REST_DENSITY = 1;
static const float EPSILON_LAMBDA = 50;
static const float EPSILON_VORTICITY = 5;
static const float C = 0.01f;
static const float K = 0.001f;
static const float deltaQMag = .2f * H;
static const float wQH = KPOLY * (H * H - deltaQMag * deltaQMag) * (H * H - deltaQMag * deltaQMag) * (H * H - deltaQMag * deltaQMag);
static float width = 31;
static float height = 500;
static float depth = 25;

ParticleSystem::ParticleSystem() : grid((int)width, (int)height, (int)depth) {
	for (int i = 0; i < 30; i++) {
		for (int j = 0; j < 30; j++) {
			for (int k = 0; k < 10; k++) {
				particles.push_back(Particle(glm::vec3(i, j, k), 1));
			}
		}
	}

	positions.reserve(particles.capacity());
}

ParticleSystem::~ParticleSystem() {}

vector<glm::vec3>& ParticleSystem::getPositions() {
	positions.clear();
	for (int i = 0; i < particles.size(); i++) {
		positions.push_back(particles[i].oldPos);
	}

	return positions;
}

void ParticleSystem::update() {
	for (auto &p : particles) {
		applyGravity(p);
		p.newPos = p.oldPos;

		//update velocity vi = vi + deltaT * fExt
		p.velocity += p.force * deltaT;

		//predict position x* = xi + deltaT * vi
		p.newPos += p.velocity * deltaT;

		imposeConstraints(p);
	}

	//get neighbors
	grid.updateCells(particles);
	for (auto &p : particles) {
		p.neighbors.clear();
		glm::ivec3 pos = p.newPos;
		vector<Cell*> neighborCells = grid.cells[pos.x][pos.y][pos.z].neighbors;
		for (auto &c : neighborCells) {
			vector<Particle*> allParticles = c->particles;
			for (auto &n : allParticles) {
				if (p.newPos != n->newPos) {
					if (glm::distance(p.newPos, n->newPos) <= H) {
						p.neighbors.push_back(n);
					}
				}
			}
		}
	}

	//while solver < iterations (2-4 enough in paper)
	for (int i = 0; i < PRESSURE_ITERATIONS; i++) {
		//set lambda
		for (auto &p : particles) {
			//vector<Particle*> neighbors = p->neighbors; //this is stupid
			p.lambda = lambda(p, p.neighbors);
		}

		//calculate deltaP
		for (auto &p : particles) {
			glm::vec3 deltaP = glm::vec3(0.0f);
			for (auto &n : p.neighbors) {
				float lambdaSum = p.lambda + n->lambda;
				float sCorr = sCorrCalc(p, n);
				deltaP += WSpiky(p.newPos, n->newPos) * (lambdaSum + sCorr);
			}

			p.deltaP = deltaP / REST_DENSITY;
		}

		//update position x*i = x*i + deltaPi
		for (auto &p : particles) {
			p.newPos += p.deltaP;
		}
	}

	for (auto &p : particles) {
		imposeConstraints(p);

		//set new velocity vi = (x*i - xi) / deltaT
		p.velocity = (p.newPos - p.oldPos) / deltaT;

		//apply vorticity confinement
		p.velocity += vorticityForce(p) * deltaT;

		//apply XSPH viscosity
		p.velocity += xsphViscosity(p);

		//update position xi = x*i
		p.oldPos = p.newPos;
	}
}

void ParticleSystem::applyGravity(Particle &p) {
	p.force = glm::vec3(0.0f);
	p.force += GRAVITY;
}

//Poly6 Kernel
float ParticleSystem::WPoly6(glm::vec3 &pi, glm::vec3 &pj) {
	glm::vec3 r = pi - pj;
	float rLen = glm::length(r);
	if (rLen > H || rLen == 0) {
		return 0;
	}

	return KPOLY * glm::pow((H * H - glm::length2(rLen)), 3);
}

//Spiky Kernel
glm::vec3 ParticleSystem::WSpiky(glm::vec3 &pi, glm::vec3 &pj) {
	glm::vec3 r = pi - pj;
	float rLen = glm::length(r);
	if (rLen > H || rLen == 0) {
		return glm::vec3(0.0f);
	}

	float coeff = (H - rLen) * (H - rLen);
	coeff *= SPIKY;
	coeff /= rLen;
	return r * -coeff;
}

//Viscosity Kernel
glm::vec3 ParticleSystem::WViscosity(glm::vec3 &pi, glm::vec3 &pj) {
	glm::vec3 r = pi - pj;
	float rLen = glm::length(r);
	if (rLen > H || rLen == 0) {
		return glm::vec3(0.0f);
	}

	float coeff = (-1 * (rLen * rLen * rLen)) / (2 * (H * H * H));
	coeff += ((rLen * rLen) / (H * H));
	coeff += (H / (2 * rLen)) - 1;
	return r * coeff;
}

//Calculate the lambda value for pressure correction
float ParticleSystem::lambda(Particle &p, vector<Particle*> &neighbors) {
	float densityConstraint = calcDensityConstraint(p, neighbors);
	glm::vec3 gradientI = glm::vec3(0.0f);
	float sumGradients = 0.0f;
	for (auto &n : neighbors) {
		//Calculate gradient with respect to j
		glm::vec3 gradientJ = glm::vec3(WSpiky(p.newPos, n->newPos) / REST_DENSITY);

		//Add magnitude squared to sum
		sumGradients += glm::length2(gradientJ);
		gradientI += gradientJ;
	}

	//Add the particle i gradient magnitude squared to sum
	sumGradients += glm::length2(gradientI);
	return ((-1) * densityConstraint) / (sumGradients + EPSILON_LAMBDA);
}

//Returns density constraint of a particle
float ParticleSystem::calcDensityConstraint(Particle &p, vector<Particle*> &neighbors) {
	float sum = 0.0f;
	for (auto &n : neighbors) {
		sum += n->mass * WPoly6(p.newPos, n->newPos);
	}

	return (sum / REST_DENSITY) - 1;
}

//Returns vorticity for a given particle
glm::vec3 ParticleSystem::vorticity(Particle &p) {
	glm::vec3 vorticity = glm::vec3(0.0f);
	glm::vec3 velocityDiff;
	glm::vec3 gradient;

	for (auto &n : p.neighbors) {
		velocityDiff = glm::vec3(n->velocity - p.velocity);
		gradient = WViscosity(p.newPos, n->newPos);
		vorticity += glm::cross(velocityDiff, gradient);
	}

	return vorticity;
}

//Returns the eta vector that points in the direction of the corrective force
glm::vec3 ParticleSystem::eta(Particle &p, float vorticityMag) {
	glm::vec3 eta = glm::vec3(0.0f);
	for (auto &n : p.neighbors) {
		eta += WViscosity(p.newPos, n->newPos) * vorticityMag;
	}

	return eta;
}

//Calculates the vorticity force for a particle
glm::vec3 ParticleSystem::vorticityForce(Particle &p) {
	glm::vec3 vorticityVal = vorticity(p);
	if (glm::length(vorticityVal) == 0.0f) {
		//No direction for eta
		return glm::vec3(0.0f);
	}

	glm::vec3 etaVal = eta(p, glm::length(vorticityVal));
	if (etaVal == glm::vec3(0.0f)) {
		return glm::vec3(0.0f);
	}
	
	glm::vec3 n = glm::normalize(etaVal);
	return (glm::cross(n, vorticityVal) * EPSILON_VORTICITY);
}

float ParticleSystem::sCorrCalc(Particle &pi, Particle* &pj) {
	// Get Density from WPoly6 and divide by constant from paper
	float corr = WPoly6(pi.newPos, pj->newPos) / wQH;
	// take to power of 4
	corr *= corr * corr * corr;
	return -K * corr;
}

glm::vec3 ParticleSystem::xsphViscosity(Particle &p) {
	glm::vec3 visc = glm::vec3(0.0f);
	for (auto &n : p.neighbors) {
		glm::vec3 velocityDiff = n->velocity - p.velocity;
		velocityDiff *= WPoly6(p.newPos, n->newPos);
	}

	return visc * C;
}

void ParticleSystem::imposeConstraints(Particle &p) {
	if (outOfRange(p.newPos.x, 0, width)) {
		p.velocity.x = 0;
	}

	if (outOfRange(p.newPos.y, 0, height)) {
		p.velocity.y = 0;
	}

	if (outOfRange(p.newPos.z, 0, depth)) {
		p.velocity.z = 0;
	}

	p.newPos.x = clampedConstraint(p.newPos.x, width);
	p.newPos.y = clampedConstraint(p.newPos.y, height);
	p.newPos.z = clampedConstraint(p.newPos.z, depth);
}

float ParticleSystem::clampedConstraint(float x, float max) {
	if (x < 0.0f) {
		return 0.0f;
	} else if (x >= max) {
		return max - 0.1f;
	} else {
		return x;
	}
}

//Tests if a particle is out of range of the box
bool ParticleSystem::outOfRange(float x, float min, float max) {
	return x < min || x >= max;
}