// This file is part of fityk program. Copyright (C) Marcin Wojdyr
// Licence: GNU General Public License ver. 2+
/// virtual machine for dataset transformations (@n = ...)

#include "transform.h"
#include "logic.h"
#include "lexer.h"
#include "data.h"

using namespace std;

namespace {

struct DtStackItem
{
    bool is_num;
    realt num;
    vector<Point> points;
    string title;
};

realt find_extrapolated_y(vector<Point> const& pp, realt x)
{
    if (pp.empty())
        return 0;
    else if (x <= pp.front().x)
        return pp.front().y;
    else if (x >= pp.back().x)
        return pp.back().y;
    vector<Point>::const_iterator i = lower_bound(pp.begin(), pp.end(),
                                                  Point(x, 0));
    assert (i > pp.begin() && i < pp.end());
    if (is_eq(x, i->x))
        return i->y;
    else
        return (i-1)->y + (i->y - (i-1)->y) * (i->x - x) / (i->x - (i-1)->x);
}

void merge_same_x(vector<Point> &pp, bool avg)
{
    int count_same = 1;
    double x0 = 0; // 0 is assigned only to avoid compiler warnings
    for (int i = pp.size() - 2; i >= 0; --i) {
        if (count_same == 1)
            x0 = pp[i+1].x;
        if (is_eq(pp[i].x, x0)) {
            pp[i].x += pp[i+1].x;
            pp[i].y += pp[i+1].y;
            pp[i].sigma += pp[i+1].sigma;
            pp[i].is_active = pp[i].is_active || pp[i+1].is_active;
            pp.erase(pp.begin() + i+1);
            count_same++;
            if (i > 0)
                continue;
            else
                i = -1; // to change pp[0]
        }
        if (count_same > 1) {
            pp[i+1].x /= count_same;
            if (avg) {
                pp[i+1].y /= count_same;
                pp[i+1].sigma /= count_same;
            }
            count_same = 1;
        }
    }
}

void shirley_bg(vector<Point> &pp)
{
    const int max_iter = 50;
    const double max_rdiff = 1e-6;
    const int n = pp.size();
    double ya = pp[0].y; // lowest bg
    double yb = pp[n-1].y; // highest bg
    double dy = yb - ya;
    vector<double> B(n, ya);
    vector<double> PA(n, 0.);
    double old_A = 0;
    for (int iter = 0; iter < max_iter; ++iter) {
        vector<double> Y(n);
        for (int i = 0; i < n; ++i)
            Y[i] = pp[i].y - B[i];
        for (int i = 1; i < n; ++i)
            PA[i] = PA[i-1] + (Y[i] + Y[i-1]) / 2 * (pp[i].x - pp[i-1].x);
        double rel_diff = old_A != 0. ? fabs(PA[n-1] - old_A) / old_A : 1.;
        if (rel_diff < max_rdiff)
            break;
        old_A = PA[n-1];
        for (int i = 0; i < n; ++i)
            B[i] = ya + dy / PA[n-1] * PA[i];
    }
    for (int i = 0; i < n; ++i)
        pp[i].y = B[i];
}

} // anonymous namespace


/// executes VM code and stores results in Dataset `out'
void DatasetTransformer::run_dt(const VMData& vm, int out)
{
    DtStackItem stack[6];
    DtStackItem* stackPtr = stack - 1; // will be ++'ed first

    v_foreach (int, i, vm.code()) {
        switch (*i) {

            case OP_NUMBER:
                stackPtr += 1;
                if (stackPtr - stack >= 6)
                    throw ExecuteError("stack overflow");
                i++;
                stackPtr->is_num = true;
                stackPtr->num = vm.numbers()[*i];
                break;

            case OP_DATASET:
                stackPtr += 1;
                if (stackPtr - stack >= 6)
                    throw ExecuteError("stack overflow");
                stackPtr->is_num = false;
                i++;
                stackPtr->points = F_->get_data(*i)->points();
                stackPtr->title = F_->get_data(*i)->get_title();
                if (stackPtr->title.empty())
                    stackPtr->title = "nt"; // no title
                break;

            case OP_NEG:
                if (stackPtr->is_num)
                    stackPtr->num = -stackPtr->num;
                else {
                    vm_foreach (Point, j, stackPtr->points)
                        j->y = -j->y;
                    stackPtr->title = "-" + stackPtr->title;
                }
                break;

            case OP_ADD:
                stackPtr -= 1;
                if (stackPtr->is_num && (stackPtr+1)->is_num)
                    stackPtr->num += (stackPtr+1)->num;
                else if (!stackPtr->is_num && !(stackPtr+1)->is_num) {
                    vm_foreach (Point, j, stackPtr->points)
                        j->y += find_extrapolated_y((stackPtr+1)->points, j->x);
                    stackPtr->title += "+" + (stackPtr+1)->title;
                }
                else
                    throw ExecuteError("adding number and dataset");
                break;

            case OP_SUB:
                stackPtr -= 1;
                if (stackPtr->is_num && (stackPtr+1)->is_num)
                    stackPtr->num -= (stackPtr+1)->num;
                else if (!stackPtr->is_num && !(stackPtr+1)->is_num) {
                    vm_foreach (Point, j, stackPtr->points)
                        j->y -= find_extrapolated_y((stackPtr+1)->points, j->x);
                    stackPtr->title += "-" + (stackPtr+1)->title;
                }
                else
                    throw ExecuteError("substracting number and dataset");
                break;

            case OP_MUL:
                stackPtr -= 1;
                if (stackPtr->is_num && (stackPtr+1)->is_num)
                    stackPtr->num *= (stackPtr+1)->num;
                else if (!stackPtr->is_num && !(stackPtr+1)->is_num) {
                    throw ExecuteError("multiplying two datasets");
                }
                else if (!stackPtr->is_num && (stackPtr+1)->is_num) {
                    realt mult = (stackPtr+1)->num;
                    vm_foreach (Point, j, stackPtr->points)
                        j->y *= mult;
                    stackPtr->title += "*" + S(mult);
                }
                else if (stackPtr->is_num && !(stackPtr+1)->is_num) {
                    realt mult = stackPtr->num;
                    stackPtr->points.swap((stackPtr+1)->points);
                    stackPtr->is_num = false;
                    vm_foreach (Point, j, stackPtr->points)
                        j->y *= mult;
                    stackPtr->title = S(mult) + "*" + (stackPtr+1)->title;
                }
                break;

            case OP_DT_SUM_SAME_X:
                if (stackPtr->is_num)
                    throw ExecuteError(op2str(*i) + " is defined only for @n");
                merge_same_x(stackPtr->points, false);
                break;

            case OP_DT_AVG_SAME_X:
                if (stackPtr->is_num)
                    throw ExecuteError(op2str(*i) + " is defined only for @n");
                merge_same_x(stackPtr->points, true);
                break;

            case OP_DT_SHIRLEY_BG:
                if (stackPtr->is_num)
                    throw ExecuteError(op2str(*i) + " is defined only for @n");
                shirley_bg(stackPtr->points);
                break;

            case OP_AND:
                // do nothing
                break;

            case OP_AFTER_AND:
                stackPtr -= 1;
                if (stackPtr->is_num || (stackPtr+1)->is_num)
                    throw ExecuteError("expected @n on both sides of `and'");
                stackPtr->points.insert(stackPtr->points.end(),
                                        (stackPtr+1)->points.begin(),
                                        (stackPtr+1)->points.end());
                sort(stackPtr->points.begin(), stackPtr->points.end());
                stackPtr->title += "&" + (stackPtr+1)->title;
                break;

            default:
                throw ExecuteError("op " + op2str(*i) +
                               " is not allowed in dataset transformations.");
        }
    }
    assert(stackPtr == stack);

    // change dataset `out'
    if (out == Lexer::kNew) {
        F_->append_dm();
        out = F_->get_dm_count() - 1;
    }
    Data *data = F_->get_data(out);
    if (!stackPtr->is_num) {
        data->set_points(stackPtr->points);
        data->set_title(stackPtr->title);
    }
    else if (stackPtr->num == 0.)
        data->clear();
    else
        throw ExecuteError("dataset or 0 expected on RHS");
}
