(set-option :incremental false)
(set-logic QF_BV)
(declare-fun s () (_ BitVec 4))
(declare-fun t () (_ BitVec 4))

(assert (not (= (bvnand s t) (bvnot (bvand s t)))))



