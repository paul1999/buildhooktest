/**
 * @module vector
 * 2D Vector class.
 * Support + - * /
 */

/**************************************************************************
*   Copyright 2018 Michael Eischer, Andreas Wendler                       *
*   Robotics Erlangen e.V.                                                *
*   http://www.robotics-erlangen.de/                                      *
*   info@robotics-erlangen.de                                             *
*                                                                         *
*   This program is free software: you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation, either version 3 of the License, or     *
*   any later version.                                                    *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
**************************************************************************/

import * as geom from "base/geom";
import * as MathUtil from "base/mathutil";

export class Vector {
	public x: number;
	public y: number;

	constructor(x: number, y: number) {
		this.x = x;
		this.y = y;
	}

	// functions used for operator overloading
	add(other: Vector): Vector {
		return new Vector(this.x + other.x, this.y + other.y);
	}

	sub(other: Vector): Vector {
		return new Vector(this.x - other.x, this.y - other.y);
	}

	mul(factor: number): Vector {
		return new Vector(this.x * factor, this.y * factor);
	}

	div(factor: number): Vector {
		return new Vector(this.x / factor, this.y / factor);
	}

	unm(): Vector {
		return new Vector(-this.x, -this.y);
	}

	addEq(other: Vector) {
		this.x += other.x;
		this.y += other.y;
	}

	subEq(other: Vector) {
		this.x -= other.x;
		this.y -= other.y;
	}

	mulEq(factor: number) {
		this.x *= factor;
		this.y *= factor;
	}

	divEq(factor: number) {
		this.x /= factor;
		this.y /= factor;
	}

	/**
	 * Creates a copy of the current vector.
	 * Doesn't copy read-only flag
	 */
	copy(): Vector {
		return new Vector(this.x, this.y);
	}

	/**
	 * Checks for invalid vector
	 * @return True if a coordinate is NaN
	 */
	isNan(): boolean {
		return isNaN(this.x) || isNaN(this.y);
	}

	/**  Get vector length */
	length(): number {
		let x = this.x;
		let y = this.y;
		return Math.sqrt(x * x + y * y);
	}

	equals(other: Vector): boolean {
		return this.x === other.x && this.y === other.y;
	}

	/** Get squared vector length */
	lengthSq(): number {
		let x = this.x;
		let y = this.y;
		return x * x + y * y;
	}

	/**
	 * Normalizes current vector.
	 * A normalized vector has the length 1.
	 * Null vector won't be modified
	 * @return reference to this
	 */
	normalize(): Vector {
		let x = this.x;
		let y = this.y;
		let l = Math.sqrt(x * x + y * y);
		if (l > 0) {
			let invLen = 1 / l;
			this.x = x * invLen;
			this.y = y * invLen;
		}
		return this;
	}

	/**
	 * Change length of current vector to given value
	 * @param len - New length of current vector
	 * @return reference to this
	 */
	setLength(len: number): Vector {
		if (len === 0) {
			this.x = 0;
			this.y = 0;
			return this;
		}
		let x = this.x;
		let y = this.y;
		let l = Math.sqrt(x * x + y * y);
		if (l > 0) {
			l = len / l;
			this.x *= l;
			this.y *= l;
		}
		return this;
	}

	/**
	 * Scale the current vectors length
	 * @param scale - factor to scale vector length with
	 * @return reference to this
	 */
	scaleLength(scale: number) {
		this.x *= scale;
		this.y *= scale;
		return this;
	}

	/**
	 * Distance between vectors.
	 * distance = (other - this).length()
	 */
	distanceTo(other: Vector): number {
		let dx = other.x - this.x;
		let dy = other.y - this.y;
		return Math.sqrt(dx * dx + dy * dy);
	}

	/**
	 * Distance between vectors squared.
	 * distance = (other - this).lengthSq()
	 */
	distanceToSq(other: Vector): number {
		let dx = other.x - this.x;
		let dy = other.y - this.y;
		return dx * dx + dy * dy;
	}

	/** Calcualates dot product */
	dot(other: Vector): number {
		return this.x * other.x + this.y * other.y;
	}

	/**
	 * Vector direction in radians
	 * @return angle in interval [-pi, +pi]
	 */
	angle(): number {
		return Math.atan2(this.y, this.x);
	}

	/**
	 * Angle from current to other vector
	 * @return angle in interval [-pi, +pi]
	 */
	angleDiff(other: Vector): number {
		if (this.lengthSq() === 0 || other.lengthSq() === 0) {
			return 0;
		}
		return geom.getAngleDiff(this.angle(), other.angle());
	}

	/**
	 * Absolute angle between current and other vector
	 * @return absolute angle in interval [0, +pi]
	 */
	absoluteAngleDiff(other: Vector): number {
		let thisLength = this.lengthSq();
		let otherLength = other.lengthSq();
		if (thisLength === 0 || otherLength === 0) {
			return 0;
		}
		return Math.acos(MathUtil.bound(-1, this.dot(other) / (Math.sqrt(thisLength) * Math.sqrt(otherLength)), 1));
	}

	/**
	 * Perpendicular to current vector.
	 * Returns perpendicular which is reached first when rotating clockwise.
	 * Equals rotate(-Math.PI/2)
	 */
	perpendicular(): Vector {
		// rotate by 90 degree cw
		return new Vector(this.y, -this.x);
	}

	/**
	 * Rotate this vector.
	 * Angles are oriented counterclockwise
	 * @param angle - angle in radians
	 */
	rotate(angle: number): Vector {
		let x = this.x;
		let y = this.y;

		this.y = Math.sin(angle) * x + Math.cos(angle) * y;
		this.x = Math.cos(angle) * x - Math.sin(angle) * y;
		return this;
	}

	/**
	 * Calculate orthogonalProjection on a given line.
	 * The line is defined by linePoint1 and linePoint2.
	 * Distance as seen from line when linePoint1 is on the left and linePoint2 on the right
	 * @param linePoint1 - point of line
	 * @param linePoint2 - point of line
	 * @return projected point
	 * @return (signed) distance to line
	 */
	orthogonalProjection(linePoint1: Vector, linePoint2: Vector): [Vector, number] {
		let rv = linePoint2 - linePoint1;
		if (rv.lengthSq() < 0.00001 * 0.00001) {
			return [linePoint1, this.distanceTo(linePoint1)];
		}
		let [is, dist] = geom.intersectLineLine(this, rv.perpendicular(), linePoint1, rv);
		if (is != undefined) {
			return [is, dist! * rv.length()];
		} else {
			return [this, 0];
		}
	}

	/**
	 * Distance value of orthogonalProjection
	 * @see orthogonalProjection
	 * @param linePoint1 - point of line
	 * @param linePoint2 - point of line
	 * @return distance to line
	 */
	orthogonalDistance(linePoint1: Vector, linePoint2: Vector): number {
		let [_, dist] = this.orthogonalProjection(linePoint1, linePoint2);
		return dist;
	}

	/**
	 * Distance to given line segment.
	 * Calculates distance from current vector to nearest point on line segment from lineStart to lineEnd
	 * @param lineStart - start of line
	 * @param lineEnd - end of line
	 */
	distanceToLineSegment(lineStart: Vector, lineEnd: Vector): number {
		let dir = (lineEnd - lineStart).normalize();
		let d = this - lineStart;
		if (d.dot(dir) < 0) {
			return d.length();
		}
		d = this - lineEnd;
		if (d.dot(dir) > 0) {
			return d.length();
		}

		// let normal = dir:perpendicular()
		// return math.abs(d:dot(normal))
		return Math.abs(d.x * dir.y - d.y * dir.x);
	}

	/**
	 * Calculates the point on a line segment with the shortest distance to a given point.
	 * The distance between the line and the point equals the result of distanceToLineSegment
	 * @param lineStart - the start point of the line
	 * @param lineEnd - the end point of the line
	 */
	nearestPosOnLine(lineStart: Vector, lineEnd: Vector): Vector {
		let dir = (lineEnd - lineStart);
		if ((this - lineStart).dot(dir) <= 0) {
			return lineStart;
		} else if ((this - lineEnd).dot(dir) >= 0) {
			return lineEnd;
		}
		// the code below this line does the same as Vector.orthogonalProjection
		let d1 = dir.x, d2 = dir.y;
		let p1 = lineStart.x, p2 = lineStart.y;
		let a1 = this.x, a2 = this.y;
		let x1 = (d1 * d1 * a1 + d1 * d2 * (a2 - p2) + d2 * d2 * p1) / (d1 * d1 + d2 * d2);
		let x2 = (d2 * d2 * a2 + d2 * d1 * (a1 - p1) + d1 * d1 * p2) / (d2 * d2 + d1 * d1);
		return new Vector(x1, x2);
	}

	complexMultiplication(other: Vector): Vector {
		return new Vector(this.x * other.x - this.y * other.y, this.x * other.y + this.y * other.x);
	}

	// used for base debug
	_toString() {
		return `Vector(${this.x.toFixed(2)}, ${this.y.toFixed(2)})`;
	}

	/**
	 * Creates a new read-only vector
	 * @see Vector.create
	 * @param x - x coordinate
	 * @param y - y coordinate
	 */
	static createReadOnly(x: number, y: number): Readonly<Vector> {
		return new Vector(x, y);
	}

	/**
	 * Create unit vector with given direction
	 * @param angle - Direction in radians
	 */
	static fromAngle(angle: number): Vector {
		return new Vector(Math.cos(angle), Math.sin(angle));
	}

	/**
	 * Creates a random point around mean with a normal distribution
	 * @param sigma - the sigma of the distribution
	 * @param mean - the middle point of the distribution
	 * @return the random point
	 */
	static random(sigma: number, mean: Vector = new Vector(0, 0)): Vector {
		let u: number, v: number, s: number;

		do {
			u = -1.0 + 2.0 * MathUtil.random();
			v = -1.0 + 2.0 * MathUtil.random();

			s = u * u + v * v;
		} while (s === 0.0 || s >= 1.0);

		// Box-Muller transform (polar)
		let tmp = sigma * Math.sqrt(-2.0 * Math.log(s) / s);

		return new Vector(tmp * u + mean.x, tmp * v + mean.y);
	}

	/**
	 * Check whether a given value is a vector
	 * @param data - The value to test
	 * @return True, if data is a vector
	 */
	static isVector(data: any): boolean {
		return typeof(data) === "object" && data.constructor.name === "Vector";
	}
}

// just alias for now
export type Position = Vector;
export type Speed = Vector;
export type RelativePosition = Vector;
