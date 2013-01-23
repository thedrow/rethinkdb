module RethinkDB
  class RQL
    @@gensym_cnt = 0
    def new_func(&b)
      args = (0...b.arity).map{@@gensym_cnt += 1}
      body = b.call(*(args.map{|i| RQL.new.var i}))
      RQL.new.func(args, body)
    end

    @@rewrites = {
      :< => :lt, :<= => :le, :> => :gt, :>= => :ge,
      :+ => :add, :- => :sub, :* => :mul, :/ => :div,
      :"|" => :any, :or => :any,
      :"&" => :all, :and => :all }
    def method_missing(m, *a, &b)
      m = @@rewrites[m] || m
      termtype = Term2::TermType.values[m.to_s.upcase.to_sym]
      unbound_if(!termtype, m)
      args = (@body ? [self] : []) + a + (b ? [new_func(&b)] : [])
      t = Term2.new
      t.type = termtype
      t.args = args.map{|x| RQL.new.expr(x).to_pb}
      return RQL.new t
    end

    def [](ind)
      if ind.class == Fixnum
        return nth(ind)
      elsif ind.class == Range
        if ind.end == 0 && ind.exclude_end?
          raise ArgumentError, "Cannot slice to an excluded end of 0."
        end
        return slice(ind.begin, ind.end - (ind.exclude_end? ? 1 : 0))
      end
      raise ArgumentError, "[] cannot handle #{ind.inspect} of type #{ind.class}."
    end

    def ==(rhs)
      raise ArgumentError,"
      Cannot use inline ==/!= with RQL queries, use .eq() instead if
      you want a query that does equality comparison.

      If you need to see whether two queries are the same, compare
      their protobufs like: `query1.to_pb == query2.to_pb`."
    end

    def do(*a, &b)
      RQL.new.funcall(new_func(&b), *((@body ? [self] : []) + a))
    end
  end
end
